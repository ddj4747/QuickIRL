#pragma once

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib/httplib.h>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class ObsControlRelay final {
public:
	using Authorize = std::function<bool(const httplib::Request &, httplib::Response &, bool)>;

	ObsControlRelay() = default;
	ObsControlRelay(const ObsControlRelay &) = delete;
	ObsControlRelay &operator=(const ObsControlRelay &) = delete;
	~ObsControlRelay();

	bool start(const std::filesystem::path &certificate, const std::filesystem::path &key, uint16_t port,
		   Authorize authorize);
	void stop();

private:
	struct PendingRequest {
		std::condition_variable changed;
		bool completed{false};
		int status{503};
		std::string body;
		std::string contentType;
	};

	void handleConnection(httplib::ws::WebSocket &socket);
	void handleResponse(const std::string &message);
	void forward(const httplib::Request &request, httplib::Response &response);
	void disconnectLocked() const;

	std::unique_ptr<httplib::SSLServer> m_server;
	std::thread m_thread;
	Authorize m_authorize;
	std::mutex m_mutex;
	httplib::ws::WebSocket *m_socket{nullptr};
	std::map<std::string, std::shared_ptr<PendingRequest>> m_pending;
	bool m_stopping{false};
};
