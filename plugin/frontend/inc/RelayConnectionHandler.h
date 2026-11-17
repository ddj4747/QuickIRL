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
	QNetworkAccessManager *m_network{nullptr};
	QNetworkReply *m_startReply{nullptr};
	QUrl m_feedUrl;
	QByteArray m_apiKey;
	QByteArray m_requestBody;
	bool m_feedCreated{false};
};
