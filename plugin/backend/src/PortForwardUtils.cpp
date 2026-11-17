#include <PortForwardUtils.h>

#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QNetworkInformation>
#include <QNetworkInterface>
#include <QUdpSocket>
#include <iostream>
#include <mutex>
#include <plugin-support.h>
#include <set>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

std::mutex PortForwarder::s_availabilityMutex;
std::condition_variable PortForwarder::s_availabilityCondition;
std::atomic<bool> PortForwarder::s_forwardingAvailable{false};
bool PortForwarder::s_initialAvailabilityChecked = false;
size_t PortForwarder::s_instanceCount = 0;
uint64_t PortForwarder::s_networkGeneration = 0;

PortForwarder::PortForwarder(const uint16_t portNumber, const Protocol protocol)
	: m_port(std::to_string(portNumber)),
	  m_protocol(protocol == Protocol::TCP ? "TCP" : "UDP"),
	  m_timer(new QTimer(this)),
	  m_networkTimer(new QTimer(this)) {
	connect(m_timer, &QTimer::timeout, this, [this]() {
		if (!m_leaseRenewFuture.isFinished()) {
			return;
		}

		m_leaseRenewFuture = QtConcurrent::run([this]() { (void)tryRenewPortLeaseUPnP(); });
	});

	{
		std::lock_guard<std::mutex> lock(s_availabilityMutex);
		if (s_instanceCount++ == 0) {
			s_forwardingAvailable.store(false);
			s_initialAvailabilityChecked = false;
			startAvailabilityCheckLocked();
		}
	}

	startNetworkMonitoring();
}

PortForwarder::~PortForwarder() {
	m_timer->stop();
	m_networkTimer->stop();
	m_forwardFuture.waitForFinished();
	m_leaseRenewFuture.waitForFinished();
	for (QFutureWatcher<void> *watcher : m_availabilityWatchers) {
		watcher->waitForFinished();
	}

	{
		std::lock_guard<std::mutex> lock(s_availabilityMutex);
		--s_instanceCount;
	}
	s_availabilityCondition.notify_all();

	if (!m_forwarded.load()) {
		return;
	}

	(void)tryClosePortUPnP();
	FreeUPNPUrls(&m_urls);
}

void PortForwarder::forward() {
	if (m_forwarded.load() || m_forwardInProgress.exchange(true)) {
		return;
	}

	m_forwardFuture = QtConcurrent::run([this]() {
		waitForInitialAvailabilityCheck();
		if (!isForwardingPossible() || !tryForwardPortUPnP()) {
			QMetaObject::invokeMethod(
				this,
				[this]() {
					m_forwardInProgress.store(false);
					emit onPortForwardFinished(false);
				},
				Qt::QueuedConnection);
			return;
		}

		m_forwarded.store(true);
		QMetaObject::invokeMethod(
			this,
			[this]() {
				m_forwardInProgress.store(false);
				m_timer->setInterval(LEASE_RENEW_INTERVAL);
				m_timer->start();

				emit onPortForwardFinished(true);
			},
			Qt::QueuedConnection);
	});
}

void PortForwarder::close() {
	(void)tryClosePortUPnP();
	m_publicAddress.clear();
	m_forwarded.store(false);
}

bool PortForwarder::isForwarded() const {
	return m_forwarded.load();
}

uint16_t PortForwarder::port() const {
	return std::stoi(m_port);
}

PortForwarder::Protocol PortForwarder::protocol() const {
	return m_protocol == "TCP" ? Protocol::TCP : Protocol::UDP;
}

std::optional<std::string> PortForwarder::publicAddress() const {
	return m_publicAddress.empty() ? std::nullopt : std::optional<std::string>(m_publicAddress);
}

uint16_t PortForwarder::getAvailablePort() {
	return 5000; // TODO implement this function
}

bool PortForwarder::isForwardingPossible() {
	return s_forwardingAvailable.load();
}

void PortForwarder::waitForInitialAvailabilityCheck() {
	std::unique_lock<std::mutex> lock(s_availabilityMutex);
	s_availabilityCondition.wait(lock, []() { return s_initialAvailabilityChecked || s_instanceCount == 0; });
}

void PortForwarder::startNetworkMonitoring() {
	m_networkInterfaces = networkInterfaces();
	connect(m_networkTimer, &QTimer::timeout, this, &PortForwarder::checkNetworkInterfaces);
	m_networkTimer->start(std::chrono::seconds(5));

	if (QNetworkInformation::loadDefaultBackend()) {
		QNetworkInformation *networkInformation = QNetworkInformation::instance();
		connect(networkInformation, &QNetworkInformation::reachabilityChanged, this,
			[this]() { checkForwardingAvailability(); });
		connect(networkInformation, &QNetworkInformation::transportMediumChanged, this,
			[this]() { checkForwardingAvailability(); });
	}
}

QStringList PortForwarder::networkInterfaces() {
	QStringList interfaces;
	for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
		if (!(interface.flags() & QNetworkInterface::IsUp) ||
		    !(interface.flags() & QNetworkInterface::IsRunning) ||
		    (interface.flags() & QNetworkInterface::IsLoopBack)) {
			continue;
		}

		for (const QNetworkAddressEntry &address : interface.addressEntries()) {
			interfaces.append(interface.name() + QLatin1Char('/') + address.ip().toString());
		}
	}

	interfaces.sort();
	return interfaces;
}

void PortForwarder::checkNetworkInterfaces() {
	const QStringList interfaces = networkInterfaces();
	if (interfaces == m_networkInterfaces) {
		return;
	}

	m_networkInterfaces = interfaces;
	checkForwardingAvailability();
}

void PortForwarder::checkForwardingAvailability() {
	std::lock_guard<std::mutex> lock(s_availabilityMutex);
	startAvailabilityCheckLocked();
}

void PortForwarder::startAvailabilityCheckLocked() {
	const uint64_t generation = ++s_networkGeneration;
	QFutureWatcher<void> *watcher = new QFutureWatcher<void>(this);
	m_availabilityWatchers.append(watcher);
	connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher]() {
		m_availabilityWatchers.removeOne(watcher);
		watcher->deleteLater();
	});
	watcher->setFuture(QtConcurrent::run([generation]() {
		UPNPUrls urls{};
		IGDdatas data{};
		std::string lanAddress;
		const bool available = discoverGateway(urls, data, lanAddress);
		if (available) {
			FreeUPNPUrls(&urls);
		}

		{
			std::lock_guard<std::mutex> lock(s_availabilityMutex);
			if (generation == s_networkGeneration) {
				s_forwardingAvailable.store(available);
			}
			s_initialAvailabilityChecked = true;
		}
		s_availabilityCondition.notify_all();
	}));
}

std::mutex PortForwarder::s_reservedPortsMutex;
std::set<uint16_t> PortForwarder::s_reservedPorts;

bool PortForwarder::udpPortFree(const uint16_t port) {
	QUdpSocket socket;
	return socket.bind(QHostAddress::AnyIPv4, port, QAbstractSocket::DontShareAddress);
}

uint16_t PortForwarder::reserveUdpPort(const uint16_t preferred) {
	std::lock_guard<std::mutex> lock(s_reservedPortsMutex);

	if (preferred != 0 && !s_reservedPorts.contains(preferred) && udpPortFree(preferred)) {
		s_reservedPorts.insert(preferred);
		return preferred;
	}

	for (int attempt = 0; attempt < 32; ++attempt) {
		QUdpSocket socket;
		if (!socket.bind(QHostAddress::AnyIPv4, 0, QAbstractSocket::DontShareAddress)) {
			return 0;
		}

		const auto port = static_cast<uint16_t>(socket.localPort());
		socket.close();
		if (port == 0 || s_reservedPorts.contains(port)) {
			continue;
		}

		s_reservedPorts.insert(port);
		return port;
	}

	return 0;
}

void PortForwarder::releaseUdpPort(const uint16_t port) {
	if (port == 0) {
		return;
	}

	std::lock_guard<std::mutex> lock(s_reservedPortsMutex);
	s_reservedPorts.erase(port);
}

bool PortForwarder::tryForwardPortUPnP() {
	if (!discoverGateway(m_urls, m_data, m_lanAddress)) {
		return false;
	}

	static const std::string name = std::format("OBS_PLUGIN_{}_{}", PLUGIN_NAME, PLUGIN_VERSION);
	static const std::string leaseDuration = std::to_string(LEASE_DURATION.count());

	const int result = UPNP_AddPortMapping(m_urls.controlURL, m_data.first.servicetype, m_port.c_str(),
					       m_port.c_str(), m_lanAddress.c_str(), name.c_str(), m_protocol.c_str(),
					       nullptr, leaseDuration.c_str());

	if (result != UPNPCOMMAND_SUCCESS) {
		FreeUPNPUrls(&m_urls);
		m_urls = {};
		return false;
	}

	char externalIPAddress[40] = {0};
	if (UPNP_GetExternalIPAddress(m_urls.controlURL, m_data.first.servicetype, externalIPAddress) ==
	    UPNPCOMMAND_SUCCESS) {
		m_publicAddress = externalIPAddress;
	}

	return true;
}

bool PortForwarder::discoverGateway(UPNPUrls &urls, IGDdatas &data, std::string &lanAddress) {
	int error = 0;

	UPNPDev *devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
	if (!devlist) {
		obs_log(LOG_INFO, "No UPnP device found or discovery error: %i", error);
		return false;
	}

	char address[64] = {0};
	const int status = UPNP_GetValidIGD(devlist, &urls, &data, address, sizeof(address));
	freeUPNPDevlist(devlist);
	if (status != 1) {
		obs_log(LOG_INFO, "No valid Internet Gateway Device found.");
		if (status != 0) {
			FreeUPNPUrls(&urls);
			urls = {};
		}
		return false;
	}

	lanAddress = address;
	return true;
}

bool PortForwarder::tryClosePortUPnP() const {
	if (!m_forwarded.load()) {
		return true;
	}

	const int result = UPNP_DeletePortMapping(m_urls.controlURL, m_data.first.servicetype, m_port.c_str(),
						  m_protocol.c_str(), nullptr);

	return (result == UPNPCOMMAND_SUCCESS);
}

bool PortForwarder::tryRenewPortLeaseUPnP() const {
	static const std::string name = std::format("{}_{}", PLUGIN_NAME, PLUGIN_VERSION);
	static const std::string leaseDuration = std::to_string(LEASE_DURATION.count());

	const int result = UPNP_AddPortMapping(m_urls.controlURL, m_data.first.servicetype, m_port.c_str(),
					       m_port.c_str(), m_lanAddress.c_str(), name.c_str(), m_protocol.c_str(),
					       nullptr, leaseDuration.c_str());

	return (result == UPNPCOMMAND_SUCCESS);
}
