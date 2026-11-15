#include <RelayConfig.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <yaml-cpp/yaml.h>

#include <map>
#include <set>
#include <string>

bool RelayConfig::load(const QString &directory, RelayConfig &config, QString &error) {
	const QDir configDir(directory);

	if (!QDir().mkpath(directory)) {
		error = QStringLiteral("Cannot create configuration directory: %1").arg(directory);
		return false;
	}

	const QString path = configDir.filePath(QStringLiteral("relay.yml"));

	if (!QFileInfo::exists(path)) {
		QSaveFile file(path);
		const QByteArray defaults = R"(# Restart the relay after changing these ports.
ports:
  api: 8080
  srtla: 5000
  srt: 8890
  rtsp: 8554
  rtp: 8000
  rtcp: 8001
  rtmp: 1935
  webrtc: 8889
  moq: 8892
  moq_quic: 8893
  media_api: 9997
)";

		if (!file.open(QIODevice::WriteOnly) || file.write(defaults) != defaults.size() || !file.commit()) {
			error = QStringLiteral("Cannot create %1: %2").arg(path, file.errorString());
			return false;
		}
	}

	QFile file(path);

	if (!file.open(QIODevice::ReadOnly)) {
		error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
		return false;
	}

	const QByteArray contents = file.readAll();

	if (file.error() != QFileDevice::NoError) {
		error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
		return false;
	}

	RelayConfig parsed;
	const std::map<std::string, uint16_t RelayConfig::*> fields = {
		{"api", &RelayConfig::apiPort},           {"srtla", &RelayConfig::srtlaPort},
		{"srt", &RelayConfig::srtPort},           {"rtsp", &RelayConfig::rtspPort},
		{"rtp", &RelayConfig::rtpPort},           {"rtcp", &RelayConfig::rtcpPort},
		{"rtmp", &RelayConfig::rtmpPort},         {"webrtc", &RelayConfig::webRtcPort},
		{"moq", &RelayConfig::moqPort},           {"moq_quic", &RelayConfig::moqQuicPort},
		{"media_api", &RelayConfig::mediaApiPort}};

	try {
		const YAML::Node root =
			YAML::Load(std::string(contents.constData(), static_cast<size_t>(contents.size())));

		if (!root.IsMap() || root.size() != 1 || !root["ports"] || !root["ports"].IsMap()) {
			error = QStringLiteral("Expected a single 'ports' mapping in %1").arg(path);
			return false;
		}

		std::set<std::string> seen;
		const QRegularExpression integerPattern(QStringLiteral("^[0-9]+$"));

		for (YAML::const_iterator entry = root["ports"].begin(); entry != root["ports"].end(); ++entry) {
			if (!entry->first.IsScalar() || !entry->second.IsScalar()) {
				error = QStringLiteral("Port names and values must be scalars in %1").arg(path);
				return false;
			}

			const std::string name = entry->first.Scalar();
			const std::map<std::string, uint16_t RelayConfig::*>::const_iterator field = fields.find(name);
			const std::string scalar = entry->second.Scalar();
			const QString value = QString::fromUtf8(scalar.data(), static_cast<qsizetype>(scalar.size()));
			bool ok = false;
			const uint port = value.toUInt(&ok);

			if (field == fields.end() || !seen.insert(name).second) {
				error = QStringLiteral("Unknown or duplicate port '%1' in %2")
						.arg(QString::fromStdString(name), path);
				return false;
			}

			if (!integerPattern.match(value).hasMatch() || !ok || port == 0 || port > 65535) {
				error = QStringLiteral("Port '%1' must be an integer between 1 and 65535 in %2")
						.arg(QString::fromStdString(name), path);
				return false;
			}

			parsed.*(field->second) = static_cast<uint16_t>(port);
		}
	} catch (const YAML::Exception &exception) {
		error = QStringLiteral("Invalid YAML in %1: %2").arg(path, QString::fromUtf8(exception.what()));
		return false;
	}

	config = parsed;
	error.clear();
	return true;
}
