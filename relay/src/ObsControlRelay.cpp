#include <ObsControlRelay.h>

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUuid>
#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

ObsControlRelay::~ObsControlRelay() {
	stop();
}

bool ObsControlRelay::start(const std::filesystem::path &certificate, const std::filesystem::path &key,
			    const uint16_t port, Authorize authorize) {
	if (m_server || !authorize) {
		return false;
	}

	m_authorize = std::move(authorize);
	m_stopping = false;
	m_server = std::make_unique<httplib::SSLServer>(certificate.string().c_str(), key.string().c_str());

	if (!m_server->is_valid()) {
		m_server.reset();
		return false;
	}

	m_server->set_payload_max_length(65536);
	m_server->set_websocket_ping_interval(20);
	m_server->set_websocket_max_missed_pongs(3);
	m_server->set_pre_routing_handler([this](const httplib::Request &request, httplib::Response &response) {
		const bool isObsConnection = request.path == "/_relay/obs";

		if (!m_authorize(request, response, isObsConnection)) {
			return httplib::Server::HandlerResponse::Handled;
		}

		return httplib::Server::HandlerResponse::Unhandled;
	});
	m_server->WebSocket("/_relay/obs", [this](const httplib::Request &, httplib::ws::WebSocket &socket) {
		handleConnection(socket);
	});

	const httplib::Server::Handler handler = [this](const httplib::Request &request, httplib::Response &response) {
		forward(request, response);
	};

	for (const char *path : {"/getBitrates", "/getSceneList", "/streamState", "/getPluginSources"}) {
		m_server->Get(path, handler);
	}

	for (const char *path : {"/setActiveScene", "/startStream", "/stopStream"}) {
		m_server->Post(path, handler);
	}

	if (!m_server->bind_to_port("0.0.0.0", port)) {
		spdlog::error("Failed to bind OBS control relay on port {}", port);
		m_server.reset();
		return false;
	}

	m_thread = std::thread([this] { m_server->listen_after_bind(); });
	return true;
}

void ObsControlRelay::handleConnection(httplib::ws::WebSocket &socket) {
	bool accepted = false;

	{
		const std::lock_guard<std::mutex> lock(m_mutex);

		if (!m_stopping && m_socket == nullptr) {
			m_socket = &socket;
			accepted = true;
		}
	}

	if (!accepted) {
		socket.close(httplib::ws::CloseStatus::PolicyViolation, "An OBS instance is already connected");
		return;
	}

	spdlog::info("OBS control connection established");
	socket.set_read_timeout(1);
	std::string message;

	while (true) {
		const httplib::ws::ReadResult result = socket.read(message);

		if (result == httplib::ws::ReadResult::Timeout) {
			const std::lock_guard<std::mutex> lock(m_mutex);

			if (m_stopping) {
				break;
			}

			continue;
		}

		if (result != httplib::ws::ReadResult::Text || message.size() > 131072) {
			break;
		}

		handleResponse(message);
	}

	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		m_socket = nullptr;
		disconnectLocked();
	}

	spdlog::info("OBS control connection closed");
}

void ObsControlRelay::handleResponse(const std::string &message) {
	QJsonParseError error;
	const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(message), &error);

	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return;
	}

	const QJsonObject object = document.object();
	const QJsonValue id = object.value(QStringLiteral("id"));
	const QJsonValue status = object.value(QStringLiteral("status"));
	const QJsonValue body = object.value(QStringLiteral("body_base64"));
	const QJsonValue contentType = object.value(QStringLiteral("content_type"));

	if (object.value(QStringLiteral("type")) != QStringLiteral("response") || !id.isString() ||
	    !status.isDouble() || status.toDouble() != status.toInt() || status.toInt() < 200 || status.toInt() > 599 ||
	    !body.isString() || !contentType.isString()) {
		return;
	}

	const QByteArray encoded = body.toString().toLatin1();
	const QByteArray::FromBase64Result decoded =
		QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
	const QByteArray type = contentType.toString().toUtf8();

	if (!decoded || decoded.decoded.size() > 65536 || type.size() > 256 || type.contains('\r') ||
	    type.contains('\n')) {
		return;
	}

	const std::lock_guard<std::mutex> lock(m_mutex);
	const std::map<std::string, std::shared_ptr<PendingRequest>>::iterator pending =
		m_pending.find(id.toString().toStdString());

	if (pending == m_pending.end() || pending->second->completed) {
		return;
	}

	pending->second->status = status.toInt();
	pending->second->body = decoded.decoded.toStdString();
	pending->second->contentType = type.toStdString();
	pending->second->completed = true;
	pending->second->changed.notify_one();
}

void ObsControlRelay::forward(const httplib::Request &request, httplib::Response &response) {
	if (request.method != "GET" && request.method != "POST") {
		response.status = 405;
		return;
	}

	const std::string id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
	const std::shared_ptr<PendingRequest> pending = std::make_shared<PendingRequest>();
	QJsonObject message;
	message.insert(QStringLiteral("type"), QStringLiteral("request"));
	message.insert(QStringLiteral("id"), QString::fromStdString(id));
	message.insert(QStringLiteral("method"), QString::fromStdString(request.method));
	message.insert(QStringLiteral("target"), QString::fromStdString(request.target));
	message.insert(QStringLiteral("content_type"),
		       QString::fromStdString(request.get_header_value("Content-Type")));
	message.insert(QStringLiteral("body_base64"),
		       QString::fromLatin1(QByteArray::fromStdString(request.body).toBase64()));
	const std::string encoded = QJsonDocument(message).toJson(QJsonDocument::Compact).toStdString();

	std::unique_lock<std::mutex> lock(m_mutex);

	if (m_stopping || m_socket == nullptr || m_pending.size() >= 32) {
		response.status = 503;
		return;
	}

	m_pending.emplace(id, pending);

	if (!m_socket->send(encoded)) {
		m_pending.erase(id);
		response.status = 503;
		return;
	}

	if (!pending->changed.wait_for(lock, std::chrono::seconds(15), [&pending] { return pending->completed; })) {
		m_pending.erase(id);
		response.status = 504;
		return;
	}

	m_pending.erase(id);
	response.status = pending->status;
	response.body = pending->body;

	if (!pending->contentType.empty()) {
		response.set_header("Content-Type", pending->contentType);
	}
}

void ObsControlRelay::disconnectLocked() const {
	for (const auto &val : m_pending | std::views::values) {
		if (!val->completed) {
			val->status = 503;
			val->completed = true;
			val->changed.notify_one();
		}
	}
}

void ObsControlRelay::stop() {
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		m_stopping = true;
		disconnectLocked();
	}

	if (m_server) {
		m_server->stop();
	}

	if (m_thread.joinable()) {
		m_thread.join();
	}

	m_server.reset();
}
