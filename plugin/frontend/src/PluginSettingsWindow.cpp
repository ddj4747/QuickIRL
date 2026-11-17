#include <PluginSettingsWindow.h>
#include <PluginFrontend.h>
#include <PluginSource.h>
#include <PortForwardUtils.h>
#include <plugin-support.h>
#include <QButtonGroup>
#include <QFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QTabWidget>
#include <QTimer>

PluginSettingsWindow::PluginSettingsWindow(QWidget *parent)
	: QWidget(parent, Qt::Window),
	  m_sourcePartitionSelector(new SourcePartitionSelector(this, 0, 10000)),
	  m_partitionScrollContent(new QWidget(this)),
	  m_partitionSettingsLayout(new QVBoxLayout(m_partitionScrollContent)),
	  m_partitionScrollArea(new QScrollArea(this)),
	  m_mainLayout(new QVBoxLayout(this)),
	  m_refreshSourcesTimer(new QTimer(this)) {

	setWindowTitle(QStringLiteral("%1 Settings").arg(PLUGIN_NAME));

	m_mainLayout->setContentsMargins(12, 12, 12, 12);
	m_mainLayout->setSpacing(12);
	m_mainLayout->setAlignment(Qt::AlignTop);

	m_partitionSettingsLayout->setContentsMargins(0, 0, 0, 0);
	m_partitionSettingsLayout->setSpacing(12);
	m_partitionSettingsLayout->setAlignment(Qt::AlignTop);

	m_partitionScrollArea->setWidget(m_partitionScrollContent);
	m_partitionScrollArea->setWidgetResizable(true);
	m_partitionScrollArea->setFrameShape(QFrame::NoFrame);
	m_partitionScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	QTabWidget *const tabs = new QTabWidget(this);
	QWidget *const generalTab = new QWidget(tabs);
	QWidget *const sceneSwitcherTab = new QWidget(tabs);
	QVBoxLayout *const generalLayout = new QVBoxLayout(generalTab);
	QGroupBox *const connectionGroup = new QGroupBox(QStringLiteral("Connection"), generalTab);
	QVBoxLayout *const connectionLayout = new QVBoxLayout(connectionGroup);
	QButtonGroup *const connectionMethods = new QButtonGroup(connectionGroup);
	QRadioButton *const upnpButton = new QRadioButton(QStringLiteral("UPnP"), connectionGroup);
	QRadioButton *const relayButton = new QRadioButton(QStringLiteral("Relay server"), connectionGroup);
	QRadioButton *const portForwardingButton = new QRadioButton(QStringLiteral("Port forwarding"), connectionGroup);

	connectionMethods->addButton(upnpButton, 0);
	connectionMethods->addButton(relayButton, 1);
	connectionMethods->addButton(portForwardingButton, 2);
	connectionLayout->addWidget(upnpButton);
	QLabel *const upnpUnavailable =
		new QLabel(QStringLiteral("UPnP is unavailable. Enable it on your router or configure a relay server."),
			   connectionGroup);
	upnpUnavailable->setWordWrap(true);
	connectionLayout->addWidget(upnpUnavailable);
	connectionLayout->addWidget(relayButton);

	QWidget *const relaySettings = new QWidget(connectionGroup);
	QFormLayout *const relayLayout = new QFormLayout(relaySettings);
	QLineEdit *const relayAddress = new QLineEdit(relaySettings);
	QLineEdit *const relayApiKey = new QLineEdit(relaySettings);
	relayAddress->setPlaceholderText(QStringLiteral("https://relay.example.com:8080"));
	relayApiKey->setEchoMode(QLineEdit::Password);
	relayLayout->addRow(QStringLiteral("Relay server URL"), relayAddress);
	relayLayout->addRow(QStringLiteral("Relay API key"), relayApiKey);
	QLabel *const relayPlaybackNotice = new QLabel(
		QStringLiteral(
			"Relay feed creation is available, but receiving relay video in OBS is not supported yet."),
		relaySettings);
	relayPlaybackNotice->setWordWrap(true);
	relayLayout->addRow(relayPlaybackNotice);
	connectionLayout->addWidget(relaySettings);
	connectionLayout->addWidget(portForwardingButton);
	QLabel *const manualForwardingUnavailable = new QLabel(
		QStringLiteral("Manual port forwarding is not available yet. Choose UPnP or configure a relay server."),
		connectionGroup);
	manualForwardingUnavailable->setWordWrap(true);
	connectionLayout->addWidget(manualForwardingUnavailable);
	generalLayout->addWidget(connectionGroup);
	generalLayout->addStretch();

	obs_data_t *const settings = PluginFrontend::get()->getSettingsObject();
	m_savedConnectionMethod = QString::fromUtf8(obs_data_get_string(settings, "connectionMethod"));
	m_savedRelayUrl = QString::fromUtf8(obs_data_get_string(settings, "relayServerUrl"));
	m_savedRelayApiKey = QString::fromUtf8(obs_data_get_string(settings, "relayServerApiKey"));
	m_refreshSourcesTimer->setSingleShot(true);
	m_refreshSourcesTimer->setInterval(750);
	connect(m_refreshSourcesTimer, &QTimer::timeout, this, &PluginSource::refreshConnections);
	relayAddress->setText(QString::fromUtf8(obs_data_get_string(settings, "relayServerUrl")));
	relayApiKey->setText(QString::fromUtf8(obs_data_get_string(settings, "relayServerApiKey")));

	const auto refreshAvailability = [settings, upnpButton, relayButton, portForwardingButton, relaySettings,
					  upnpUnavailable, manualForwardingUnavailable]() {
		if (!PluginFrontend::isRunning()) {
			return;
		}

		const bool available = PortForwarder::isForwardingPossible();
		upnpButton->setEnabled(available);
		upnpUnavailable->setVisible(!available);

		const bool hasSelection = obs_data_has_user_value(settings, "connectionMethod");
		const QString method = QString::fromUtf8(obs_data_get_string(settings, "connectionMethod"));

		if ((!hasSelection || method == QStringLiteral("upnp")) && available) {
			upnpButton->setChecked(true);
		} else if (method == QStringLiteral("port_forwarding")) {
			portForwardingButton->setChecked(true);
		} else {
			relayButton->setChecked(true);
		}

		relaySettings->setVisible(relayButton->isChecked());
		manualForwardingUnavailable->setVisible(portForwardingButton->isChecked());
	};

	refreshAvailability();

	connect(connectionMethods, &QButtonGroup::idClicked, this,
		[this, settings, relaySettings, relayButton, manualForwardingUnavailable](const int id) {
			if (!PluginFrontend::isRunning()) {
				return;
			}

			const char *const method = id == 0 ? "upnp" : (id == 1 ? "relay" : "port_forwarding");
			obs_data_set_string(settings, "connectionMethod", method);
			relaySettings->setVisible(relayButton->isChecked());
			manualForwardingUnavailable->setVisible(id == 2);
			saveConnectionSettings();
		});
	QTimer *const relaySettingsSaveTimer = new QTimer(generalTab);
	relaySettingsSaveTimer->setSingleShot(true);
	relaySettingsSaveTimer->setInterval(500);
	connect(relaySettingsSaveTimer, &QTimer::timeout, this, [this]() {
		if (PluginFrontend::isRunning()) {
			saveConnectionSettings();
		}
	});

	connect(relayAddress, &QLineEdit::textChanged, this, [settings, relaySettingsSaveTimer](const QString &text) {
		if (!PluginFrontend::isRunning()) {
			return;
		}

		obs_data_set_string(settings, "relayServerUrl", text.toUtf8().constData());
		relaySettingsSaveTimer->start();
	});
	connect(relayApiKey, &QLineEdit::textChanged, this, [settings, relaySettingsSaveTimer](const QString &text) {
		if (!PluginFrontend::isRunning()) {
			return;
		}

		obs_data_set_string(settings, "relayServerApiKey", text.toUtf8().constData());
		relaySettingsSaveTimer->start();
	});
	const auto saveRelaySettings = [this, relaySettingsSaveTimer]() {
		relaySettingsSaveTimer->stop();
		if (PluginFrontend::isRunning()) {
			saveConnectionSettings();
		}
	};
	connect(relayAddress, &QLineEdit::editingFinished, this, saveRelaySettings);
	connect(relayApiKey, &QLineEdit::editingFinished, this, saveRelaySettings);

	QTimer *const availabilityTimer = new QTimer(generalTab);
	connect(availabilityTimer, &QTimer::timeout, generalTab, refreshAvailability);
	availabilityTimer->start(std::chrono::seconds(1));

	QVBoxLayout *const sceneSwitcherLayout = new QVBoxLayout(sceneSwitcherTab);
	sceneSwitcherLayout->addWidget(m_sourcePartitionSelector);
	sceneSwitcherLayout->addWidget(m_partitionScrollArea, 1);

	tabs->addTab(generalTab, QStringLiteral("General"));
	tabs->addTab(sceneSwitcherTab, QStringLiteral("Scene Switcher"));
	m_mainLayout->addWidget(tabs, 1);

	setMinimumWidth(700);
	setMinimumHeight(600);

	connect(m_sourcePartitionSelector, &SourcePartitionSelector::onValuesChanged, this,
		&PluginSettingsWindow::onPartitionSelectorValueChanged);
	connect(m_sourcePartitionSelector, &SourcePartitionSelector::onPartitionClicked, this,
		&PluginSettingsWindow::onPartitionClicked);

	onPartitionSelectorValueChanged();
	onPartitionClicked(0);

	m_sourcePartitionSelector->setMinimumWidth(500);
	setLayout(m_mainLayout);
}

PluginSettingsWindow::~PluginSettingsWindow() = default;

void PluginSettingsWindow::saveConnectionSettings() {
	obs_data_t *const settings = PluginFrontend::get()->getSettingsObject();
	const QString method = QString::fromUtf8(obs_data_get_string(settings, "connectionMethod"));
	const QString relayUrl = QString::fromUtf8(obs_data_get_string(settings, "relayServerUrl"));
	const QString relayApiKey = QString::fromUtf8(obs_data_get_string(settings, "relayServerApiKey"));
	const bool relayChanged = relayUrl != m_savedRelayUrl || relayApiKey != m_savedRelayApiKey;
	const bool connectionChanged = method != m_savedConnectionMethod ||
				       (relayChanged && (method == QStringLiteral("relay") ||
							 (method.isEmpty() && !PortForwarder::isForwardingPossible())));

	m_savedConnectionMethod = method;
	m_savedRelayUrl = relayUrl;
	m_savedRelayApiKey = relayApiKey;
	PluginFrontend::get()->saveSettingsObject();

	if (connectionChanged) {
		m_refreshSourcesTimer->start();
	}
}

void PluginSettingsWindow::onPartitionSelectorValueChanged() {
	const QList<int> values = m_sourcePartitionSelector->getHandleValues();
	const qsizetype targetCount = values.count() + 1;

	while (m_partitionSettings.count() > targetCount) {
		PartitionSettingsWidget *widget = m_partitionSettings.back();
		m_partitionSettingsLayout->removeWidget(widget);

		disconnect(m_partitionSettings.back(), &PartitionSettingsWidget::onPartitionFocused, this,
			   &PluginSettingsWindow::onPartitionFocused);

		m_partitionSettings.pop_back();
		widget->deleteLater();
	}

	while (m_partitionSettings.count() < targetCount) {
		const int index = static_cast<int>(m_partitionSettings.count());
		const int min = index == 0 ? 0 : values.at(index - 1);
		const int max = index == values.count() ? 10000 : values.at(index);

		m_partitionSettings.append(new PartitionSettingsWidget(m_partitionScrollContent,
								       m_sourcePartitionSelector, index, min, max));
		connect(m_partitionSettings.back(), &PartitionSettingsWidget::onPartitionFocused, this,
			&PluginSettingsWindow::onPartitionFocused);

		m_partitionSettingsLayout->addWidget(m_partitionSettings.back());
	}

	for (PartitionSettingsWidget *const partition : m_partitionSettings) {
		partition->updateValue(10000);
	}
}

void PluginSettingsWindow::setFocusedPartition(const int partition) {
	if (partition < 0 || partition >= m_partitionSettings.count()) {
		return;
	}

	if (partition == m_focusedPartitionSettings) {
		m_partitionSettings.at(partition)->setHighlighted(true);
		m_sourcePartitionSelector->highlightPartition(partition);
		return;
	}

	if (m_focusedPartitionSettings >= 0 && m_focusedPartitionSettings < m_partitionSettings.count()) {
		m_partitionSettings.at(m_focusedPartitionSettings)->setHighlighted(false);
	}

	m_focusedPartitionSettings = partition;
	m_partitionSettings.at(partition)->setHighlighted(true);
	m_sourcePartitionSelector->highlightPartition(partition);
}

void PluginSettingsWindow::onPartitionClicked(const int partition) {
	if (partition < 0 || partition >= m_partitionSettings.count()) {
		return;
	}

	m_ignoreHoverFocus = true;
	setFocusedPartition(partition);
	m_partitionScrollArea->ensureWidgetVisible(m_partitionSettings.at(partition));
	QTimer::singleShot(0, this, [this]() { m_ignoreHoverFocus = false; });
}

void PluginSettingsWindow::onPartitionFocused(const int partition) {
	if (m_ignoreHoverFocus) {
		return;
	}

	setFocusedPartition(partition);
}
