#pragma once

#include <ConnectionHandler.h>
#include <QByteArray>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

class RelayConnectionHandler final : public ConnectionHandler {
public:
	using ConnectionHandler::ConnectionHandler;
	~RelayConnectionHandler() override;
	void start(const std::string &streamId, StreamProtocol protocol) override;
	void stop() override;
	uint16_t localSrtPort() const override;

private:
	void sendStartRequest();
	QNetworkAccessManager *m_network{nullptr};
	QNetworkReply *m_startReply{nullptr};
	QUrl m_feedUrl;
	QByteArray m_apiKey;
	QByteArray m_requestBody;
	std::string m_streamId;
	int m_startAttempts{0};
	bool m_feedCreated{false};
};
