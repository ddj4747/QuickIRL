#include <UpnpConnectionHandler.h>
#include <GoIRLStreamHandler.h>
#include <MediamtxStreamHandler.h>
#include <algorithm>
#include <optional>

UpnpConnectionHandler::~UpnpConnectionHandler() {
	stop();
}

std::vector<UpnpConnectionHandler::ConnectionPort>
UpnpConnectionHandler::forwardingPorts(const StreamProtocol protocol) {
	using Protocol = PortForwarder::Protocol;

	switch (protocol) {
	case StreamProtocol::SRTLA:
		return {{5000, Protocol::UDP}};

	case StreamProtocol::SRT:
		return {{8890, Protocol::UDP}};

	case StreamProtocol::RTMP:
		return {{1935, Protocol::TCP}};

	case StreamProtocol::RTSP:
		return {{8554, Protocol::TCP}, {8000, Protocol::UDP}, {8001, Protocol::UDP}};

	case StreamProtocol::WebRTC:
		return {{8889, Protocol::TCP}, {8189, Protocol::UDP}};

	case StreamProtocol::MoQ:
		return {{8892, Protocol::TCP}, {8892, Protocol::UDP}, {8893, Protocol::UDP}};

	default:
		return {{PortForwarder::getAvailablePort(), Protocol::UDP}};
	}
}

void UpnpConnectionHandler::start(const std::string &streamId, const StreamProtocol protocol) {
	m_streamId = streamId;
	m_protocol = protocol;

	const std::vector<ConnectionPort> ports = forwardingPorts(protocol);

	if (ports.empty() || std::ranges::any_of(ports, [](const ConnectionPort &port) { return port.port == 0; })) {
		emit streamError(QString::fromStdString(m_streamId), QStringLiteral("No forwarding ports configured"));

		return;
	}

	m_pending = ports.size();

	for (const ConnectionPort &config : ports) {
		PortForwarder *const forwarder = new PortForwarder(config.port, config.protocol);
		forwarder->setParent(this);
		m_forwarders.push_back(forwarder);

		connect(forwarder, &PortForwarder::onPortForwardFinished, this, [this](const bool success) {
			if (m_pending == 0) {
				return;
			}

			if (!success || !PortForwarder::isForwardingPossible()) {
				m_pending = 0;
				emit streamError(
					QString::fromStdString(m_streamId),
					QStringLiteral(
						"UPnP port forwarding failed. Enable UPnP on your router or configure a relay server in the plugin connection settings."));

				return;
			}

			if (--m_pending == 0) {
				startStream();
			}
		});
	}

	for (PortForwarder *forwarder : m_forwarders) {
		forwarder->forward();
	}
}

void UpnpConnectionHandler::startStream() {
	const std::optional<std::string> address = m_forwarders.front()->publicAddress();

	if (!address) {
		emit streamError(
			QString::fromStdString(m_streamId),
			QStringLiteral(
				"The router did not provide a public address. Check UPnP or configure a relay server in the plugin connection settings."));

		return;
	}

	m_streamHandler = m_protocol == StreamProtocol::SRTLA
				  ? static_cast<StreamHandler *>(new GoIRLStreamHandler(this))
				  : static_cast<StreamHandler *>(new MediamtxStreamHandler(this));
	connect(m_streamHandler, &StreamHandler::streamReady, this, &ConnectionHandler::streamReady);
	connect(m_streamHandler, &StreamHandler::streamAvailable, this, &ConnectionHandler::streamAvailable);
	connect(m_streamHandler, &StreamHandler::streamUnavailable, this, &ConnectionHandler::streamUnavailable);
	connect(m_streamHandler, &StreamHandler::streamStopped, this, &ConnectionHandler::streamStopped);
	connect(m_streamHandler, &StreamHandler::streamError, this, &ConnectionHandler::streamError);
	m_streamHandler->start(m_streamId, m_protocol, *address, m_forwarders.front()->port());
}

void UpnpConnectionHandler::stop() {
	m_pending = 0;

	if (m_streamHandler) {
		m_streamHandler->stop(m_streamId);
		delete m_streamHandler;
		m_streamHandler = nullptr;
	}

	for (PortForwarder *forwarder : m_forwarders) {
		delete forwarder;
	}

	m_forwarders.clear();
}

uint16_t UpnpConnectionHandler::localSrtPort() const {
	const GoIRLStreamHandler *const handler = dynamic_cast<const GoIRLStreamHandler *>(m_streamHandler);
	return handler ? handler->localSrtPort() : 0;
}
