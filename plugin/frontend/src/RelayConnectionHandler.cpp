#include <RelayConnectionHandler.h>
#include <PluginFrontend.h>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <magic_enum/magic_enum.hpp>
#include <chrono>

RelayConnectionHandler::~RelayConnectionHandler() {
	stop();
}

void RelayConnectionHandler::start(const std::string &streamId, const StreamProtocol protocol) {
	obs_data_t *const settings = PluginFrontend::acquireSettingsObject();
	QString relayAddress = QString::fromUtf8(obs_data_get_string(settings, "relayServerUrl")).trimmed();
	m_apiKey = QByteArray(obs_data_get_string(settings, "relayServerApiKey")).trimmed();
	obs_data_release(settings);

	if (relayAddress.isEmpty()) {
		emit streamError(QString::fromStdString(streamId),
				 QStringLiteral("Enter a relay server URL in the plugin connection settings."));

		return;
	}

	if (!relayAddress.contains(QStringLiteral("://"))) {
		relayAddress.prepend(QStringLiteral("https://"));
	}

	const QUrl urlInput(relayAddress);
	const QString scheme = urlInput.scheme();
	if (!urlInput.isValid() ||
	    (scheme.compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 &&
	     scheme.compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0) ||
	    urlInput.host().isEmpty()) {
		emit streamError(
			QString::fromStdString(streamId),
			QStringLiteral(
				"Enter a valid HTTP or HTTPS relay server URL in the plugin connection settings."));

		return;
	}

	if (m_apiKey.isEmpty()) {
		emit streamError(QString::fromStdString(streamId),
				 QStringLiteral("Enter the relay server API key in the plugin connection settings."));

		return;
	}

	QUrl url(urlInput);
	url.setPath(url.path().trimmed().remove(QRegularExpression(QStringLiteral("/+$"))) + QStringLiteral("/feeds"));
	m_feedUrl = url;

	QJsonObject body;
	body.insert(QStringLiteral("protocol"), QString::fromStdString(std::string(magic_enum::enum_name(protocol))));
	body.insert(QStringLiteral("streamKey"), QString::fromStdString(streamId));
	m_requestBody = QJsonDocument(body).toJson(QJsonDocument::Compact);

	m_network = new QNetworkAccessManager(this);
	m_streamId = streamId;
	m_startAttempts = 0;
	sendStartRequest();
}

void RelayConnectionHandler::sendStartRequest() {
	++m_startAttempts;
	QNetworkRequest request(m_feedUrl);
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	request.setRawHeader("Authorization", m_apiKey);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(10000);
	m_startReply = m_network->post(request, m_requestBody);

	QNetworkReply *const reply = m_startReply;
	QNetworkAccessManager *const network = m_network;
	const QUrl feedUrl = m_feedUrl;
	const QByteArray apiKey = m_apiKey;
	const QByteArray requestBody = m_requestBody;
	const std::string streamId = m_streamId;
	const QPointer<RelayConnectionHandler> self(this);

	connect(reply, &QNetworkReply::finished, network, [self, reply, network, feedUrl, apiKey, requestBody, streamId]() {
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray response = reply->readAll();
		const QString error = reply->errorString();
		reply->deleteLater();

		if (!self || self->m_startReply != reply) {
			if (status == 202) {
				QNetworkRequest request(feedUrl);
				request.setHeader(QNetworkRequest::ContentTypeHeader,
						  QStringLiteral("application/json"));
				request.setRawHeader("Authorization", apiKey);
				request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
						     QNetworkRequest::ManualRedirectPolicy);
				request.setTransferTimeout(10000);
				QNetworkReply *const deletion =
					network->sendCustomRequest(request, "DELETE", requestBody);
				connect(deletion, &QNetworkReply::finished, network, &QObject::deleteLater);
			} else {
				network->deleteLater();
			}

			return;
		}

		self->m_startReply = nullptr;

		if (status != 202) {
			if (status == 409 && self->m_startAttempts < 20) {
				QTimer::singleShot(std::chrono::milliseconds(500), self, [self]() {
					if (self && self->m_network) {
						self->sendStartRequest();
					}
				});

				return;
			}

			if (status == 401) {
				emit self->streamError(
					QString::fromStdString(streamId),
					QStringLiteral(
						"The relay rejected the API key. Check the key in the plugin connection settings."));

				return;
			}

			emit self->streamError(
				QString::fromStdString(streamId),
				QStringLiteral("Relay feed request failed (%1): %2").arg(status).arg(error));

			return;
		}

		self->m_feedCreated = true;
		const QString link =
			QJsonDocument::fromJson(response).object().value(QStringLiteral("link")).toString();

		if (link.isEmpty()) {
			emit self->streamError(QString::fromStdString(streamId),
					       QStringLiteral("Relay returned no feed link"));

			return;
		}

		emit self->streamReady(QString::fromStdString(streamId), link);
	});
}

void RelayConnectionHandler::stop() {
	if (!m_network) {
		return;
	}

	QNetworkAccessManager *const network = m_network;
	m_network = nullptr;
	network->setParent(QCoreApplication::instance());

	if (m_startReply) {
		m_startReply = nullptr;

		return;
	}

	if (m_feedCreated) {
		m_feedCreated = false;
		QNetworkRequest request(m_feedUrl);
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
		request.setRawHeader("Authorization", m_apiKey);
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
		request.setTransferTimeout(10000);
		QNetworkReply *const reply = network->sendCustomRequest(request, "DELETE", m_requestBody);
		connect(reply, &QNetworkReply::finished, network, &QObject::deleteLater);
	} else {
		network->deleteLater();
	}
}

uint16_t RelayConnectionHandler::localSrtPort() const {
	return 0;
}
