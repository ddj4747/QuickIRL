#pragma once

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib/httplib.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <QString>
#include <FeedContext.h>
#include <RelayConfig.h>
#include <ObsControlRelay.h>
#include <QProcess>
#include <QTimer>

class API_Endpoint final {
public:
	~API_Endpoint();
	API_Endpoint(const API_Endpoint &) = delete;
	API_Endpoint &operator=(const API_Endpoint &) = delete;

	static API_Endpoint &getInstance();
	static bool postToEventLoop(std::function<void()> operation);
	static bool prepareAccessKey(const QString &configDirectory, bool regenerate, QString &key, QString &error,
				     bool &generated, const QString &fileName = QStringLiteral("api-key"));
	bool start(const std::filesystem::path &configDirectory, const RelayConfig &config);
	void stop();

private:
	API_Endpoint() = default;
	static bool readAccessKey(const QString &path, QString &key, QString &error);
	static bool parseFeed(const httplib::Request &req, std::string &protocol, std::string &streamKey);
	static StreamProtocol feedProtocol(const std::string &protocol);
	void startFeed(const std::string &protocol, const std::string &streamKey, const std::string &host);
	void stopFeed(const std::string &streamKey);
	void refreshPublicIp();
	void registerEndpoints();
	bool authorize(const httplib::Request &req, httplib::Response &res) const;
	static bool authorizeKey(const httplib::Request &req, httplib::Response &res, const QString &path);

	std::unique_ptr<httplib::SSLServer> m_server;
	std::thread m_httpThread;
	QString m_keyPath;
	QString m_obsKeyPath;
	std::unique_ptr<ObsControlRelay> m_obsControl;
	std::string m_publicHost;
	std::unique_ptr<QProcess> m_publicIpProcess;
	std::unique_ptr<QTimer> m_publicIpTimer;

	struct FeedInfo {
		std::string protocol;
		std::string status;
		std::string error;
		std::string link;
	};

	std::unique_ptr<MediamtxManager> m_mediaManager;
	QString m_goIrlPath;
	RelayConfig m_config;
	std::map<std::string, FeedContext *> m_contexts;
	std::mutex m_feedsMutex;
	std::map<std::string, FeedInfo> m_feeds;
};
