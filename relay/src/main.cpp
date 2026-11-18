#include <API_Endpoint.h>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char **argv) {
	const QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName(QStringLiteral("obs-relay"));

	QCommandLineParser parser;
	parser.setApplicationDescription(QStringLiteral("OBS Toolkit relay server"));
	parser.addHelpOption();

	const QCommandLineOption generateApiKey(
		QStringLiteral("generate-api-key"),
		QStringLiteral("Generate and save a new API key, then print it to standard output."));

	const QCommandLineOption generateObsKey(
		QStringLiteral("generate-obs-key"),
		QStringLiteral("Generate and save a new OBS connection key, then print it to standard output."));

	parser.addOption(generateApiKey);
	parser.addOption(generateObsKey);
	parser.process(app);

	const QString configDirectory =
		qEnvironmentVariable("RELAY_CONFIG_DIR", QCoreApplication::applicationDirPath());
	if (parser.isSet(generateApiKey) && parser.isSet(generateObsKey)) {
		QTextStream(stderr) << "Choose only one key to regenerate" << Qt::endl;
		return 1;
	}

	QString key;
	QString error;
	bool generated = false;

	if (!API_Endpoint::prepareAccessKey(configDirectory, parser.isSet(generateApiKey), key, error, generated)) {
		QTextStream(stderr) << "Failed to prepare relay API key: " << error << Qt::endl;
		return 1;
	}

	if (generated) {
		if (parser.isSet(generateApiKey)) {
			QTextStream(stdout) << key << Qt::endl;
		} else {
			QTextStream(stdout) << "Relay API key saved to " << configDirectory << "/api-key" << Qt::endl;
		}
	}

	if (parser.isSet(generateApiKey)) {
		return 0;
	}

	if (!API_Endpoint::prepareAccessKey(configDirectory, parser.isSet(generateObsKey), key, error, generated,
					    QStringLiteral("obs-key"))) {
		QTextStream(stderr) << "Failed to prepare OBS connection key: " << error << Qt::endl;
		return 1;
	}

	if (generated) {
		if (parser.isSet(generateObsKey)) {
			QTextStream(stdout) << key << Qt::endl;
		} else {
			QTextStream(stdout)
				<< "OBS connection key saved to " << configDirectory << "/obs-key" << Qt::endl;
		}
	}

	if (parser.isSet(generateObsKey)) {
		return 0;
	}

	RelayConfig config;

	if (!RelayConfig::load(configDirectory, config, error)) {
		QTextStream(stderr) << "Failed to load relay configuration: " << error << Qt::endl;
		return 1;
	}

	API_Endpoint &api = API_Endpoint::getInstance();
#ifdef _WIN32
	const std::filesystem::path apiDirectory(configDirectory.toStdWString());
#else
	const std::filesystem::path apiDirectory(configDirectory.toStdString());
#endif

	if (!api.start(apiDirectory, config)) {
		QTextStream(stderr) << "Failed to start relay API server" << Qt::endl;
		return 1;
	}

	const int result = QCoreApplication::exec();
	api.stop();

	return result;
}
