#pragma once

#include <MediamtxManager.h>
#include <QObject>
#include <QString>
#include <cstdint>
#include <string>

class ConnectionHandler : public QObject {
	Q_OBJECT
public:
	using QObject::QObject;
	~ConnectionHandler() override = default;
	virtual void start(const std::string &streamId, StreamProtocol protocol) = 0;
	virtual void stop() = 0;
	virtual uint16_t localSrtPort() const = 0;

signals:
	void streamReady(const QString &streamId, const QString &publishUrl);
	void streamAvailable(const QString &streamId);
	void streamUnavailable(const QString &streamId);
	void streamStopped(const QString &streamId);
	void streamError(const QString &streamId, const QString &error);
};
