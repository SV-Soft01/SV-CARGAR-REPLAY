#include "replay-dock.hpp"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>

#include <cmath>

namespace bbr {

namespace {

inline constexpr const char *kConfigSection = "BasketballReplay";

} // namespace

ReplayDock::ReplayDock(QWidget *parent) : QDockWidget(parent)
{
	setObjectName(QStringLiteral("ObsBasketballReplayDock"));
	setWindowTitle(QStringLiteral("Basketball Replay A/B"));
	setAllowedAreas(Qt::AllDockWidgetAreas);

	setupUi();
	setupStyles();

	// Conexión del scanner
	connect(&m_scanner, &ClipScanner::clipsUpdated, this, &ReplayDock::onClipsUpdated);

	// Conexión del motor
	connect(&m_engine, &ReplayEngine::deckChanged, this, &ReplayDock::onEngineDeckChanged);
	connect(&m_engine, &ReplayEngine::sceneStateChanged, this, &ReplayDock::onEngineSceneStateChanged);
	connect(&m_engine, &ReplayEngine::statusMessage, this, &ReplayDock::onEngineStatusMessage);

	// UI Tick timer (para actualizar scrubbing, timeline y reloj a 20 fps)
	m_uiTimer.setInterval(50);
	connect(&m_uiTimer, &QTimer::timeout, this, &ReplayDock::onUiTick);
	m_uiTimer.start();
}

ReplayDock::~ReplayDock()
{
	saveSettings();
	m_uiTimer.stop();
	m_scanner.stop();
	m_engine.shutdown();
}

void ReplayDock::init()
{
	loadSettings();
	m_scanner.start();
	refreshSceneList();
	m_engine.setReady(true);
	m_engine.ensureDecks();
	updateDeckStatusBadges();
	updateTransportUi(m_activeDeckTab);
}

void ReplayDock::setupUi()
{
	QWidget *mainWidget = new QWidget(this);
	QVBoxLayout *mainLayout = new QVBoxLayout(mainWidget);
	mainLayout->setContentsMargins(6, 6, 6, 6);
	mainLayout->setSpacing(5);

	// -------------------------------------------------------------------------
	// 1. Barra Superior: Carpeta de Clips y Escena en Vivo
	// -------------------------------------------------------------------------
	QHBoxLayout *topBar = new QHBoxLayout();
	topBar->setSpacing(4);

	QLabel *lblFolder = new QLabel(QStringLiteral("Carpeta:"), mainWidget);
	m_txtFolder = new QLineEdit(mainWidget);
	m_txtFolder->setPlaceholderText(QStringLiteral("Ruta a grabaciones o buffer de repetición..."));
	m_txtFolder->setReadOnly(true);

	m_btnBrowse = new QPushButton(QStringLiteral("Examinar..."), mainWidget);
	m_btnBrowse->setMaximumWidth(85);
	connect(m_btnBrowse, &QPushButton::clicked, this, &ReplayDock::onBrowseFolder);

	QLabel *lblLive = new QLabel(QStringLiteral("En Vivo:"), mainWidget);
	m_comboLiveScene = new QComboBox(mainWidget);
	m_comboLiveScene->setMinimumWidth(130);
	connect(m_comboLiveScene, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&ReplayDock::onLiveSceneComboChanged);

	m_btnReturnLive = new QPushButton(QStringLiteral("VOLVER A VIVO"), mainWidget);
	m_btnReturnLive->setObjectName(QStringLiteral("btnReturnLive"));
	m_btnReturnLive->setStyleSheet(QStringLiteral("QPushButton { background-color: #b71c1c; color: white; font-weight: bold; padding: 4px 10px; border-radius: 3px; } QPushButton:hover { background-color: #d32f2f; }"));
	connect(m_btnReturnLive, &QPushButton::clicked, this, &ReplayDock::onReturnLiveClicked);

	topBar->addWidget(lblFolder);
	topBar->addWidget(m_txtFolder, 1);
	topBar->addWidget(m_btnBrowse);
	topBar->addSpacing(10);
	topBar->addWidget(lblLive);
	topBar->addWidget(m_comboLiveScene);
	topBar->addWidget(m_btnReturnLive);

	mainLayout->addLayout(topBar);

	// -------------------------------------------------------------------------
	// 2. Filtros Rápidos Superiores
	// -------------------------------------------------------------------------
	QHBoxLayout *filterBar = new QHBoxLayout();
	filterBar->setSpacing(4);

	m_comboCameraFilter = new QComboBox(mainWidget);
	m_comboCameraFilter->addItem(QStringLiteral("Todas las cámaras"), -1);
	m_comboCameraFilter->addItem(QStringLiteral("Principal (Buffer)"), 0);
	m_comboCameraFilter->addItem(QStringLiteral("Cámara 1 (_CAM1)"), 1);
	m_comboCameraFilter->addItem(QStringLiteral("Cámara 2 (_CAM2)"), 2);
	m_comboCameraFilter->addItem(QStringLiteral("Cámara 3 (_CAM3)"), 3);
	m_comboCameraFilter->addItem(QStringLiteral("Cámara 4 (_CAM4)"), 4);
	connect(m_comboCameraFilter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&ReplayDock::onFilterChanged);

	m_filterTeamGroup = new QButtonGroup(this);
	m_btnFilterAll = new QPushButton(QStringLiteral("Todos"), mainWidget);
	m_btnFilterAll->setCheckable(true);
	m_btnFilterAll->setChecked(true);
	m_btnFilterHome = new QPushButton(QStringLiteral("Casa [C]"), mainWidget);
	m_btnFilterHome->setCheckable(true);
	m_btnFilterAway = new QPushButton(QStringLiteral("Visitante [V]"), mainWidget);
	m_btnFilterAway->setCheckable(true);
	m_filterTeamGroup->addButton(m_btnFilterAll, 0);
	m_filterTeamGroup->addButton(m_btnFilterHome, 1);
	m_filterTeamGroup->addButton(m_btnFilterAway, 2);
	connect(m_filterTeamGroup, &QButtonGroup::idClicked, this, &ReplayDock::onFilterChanged);

	m_btnFilter3PT = new QPushButton(QStringLiteral("3PT"), mainWidget);
	m_btnFilter3PT->setCheckable(true);
	connect(m_btnFilter3PT, &QPushButton::toggled, this, &ReplayDock::onFilterChanged);

	m_btnFilterTop = new QPushButton(QStringLiteral("TOP"), mainWidget);
	m_btnFilterTop->setCheckable(true);
	connect(m_btnFilterTop, &QPushButton::toggled, this, &ReplayDock::onFilterChanged);

	m_btnFilterVar = new QPushButton(QStringLiteral("VAR"), mainWidget);
	m_btnFilterVar->setCheckable(true);
	connect(m_btnFilterVar, &QPushButton::toggled, this, &ReplayDock::onFilterChanged);

	m_btnHalftimeSummary = new QPushButton(QStringLiteral("Resumen Medio Tiempo (Playlist)"), mainWidget);
	m_btnHalftimeSummary->setStyleSheet(QStringLiteral("QPushButton { background-color: #0d47a1; color: white; font-weight: bold; } QPushButton:hover { background-color: #1976d2; }"));
	connect(m_btnHalftimeSummary, &QPushButton::clicked, this, &ReplayDock::onHalftimeSummaryClicked);

	filterBar->addWidget(new QLabel(QStringLiteral("Filtro:"), mainWidget));
	filterBar->addWidget(m_comboCameraFilter);
	filterBar->addWidget(m_btnFilterAll);
	filterBar->addWidget(m_btnFilterHome);
	filterBar->addWidget(m_btnFilterAway);
	filterBar->addSpacing(6);
	filterBar->addWidget(m_btnFilter3PT);
	filterBar->addWidget(m_btnFilterTop);
	filterBar->addWidget(m_btnFilterVar);
	filterBar->addStretch(1);
	filterBar->addWidget(m_btnHalftimeSummary);

	mainLayout->addLayout(filterBar);

	// -------------------------------------------------------------------------
	// 3. Tabla Central de Clips
	// -------------------------------------------------------------------------
	m_tableClips = new QTableWidget(mainWidget);
	m_tableClips->setColumnCount(4);
	m_tableClips->setHorizontalHeaderLabels({QStringLiteral("Nº"), QStringLiteral("Hora / Clip"), QStringLiteral("Cámara"), QStringLiteral("Etiquetas")});
	m_tableClips->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	m_tableClips->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	m_tableClips->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	m_tableClips->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	m_tableClips->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_tableClips->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_tableClips->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_tableClips->setAlternatingRowColors(true);
	connect(m_tableClips, &QTableWidget::itemSelectionChanged, this, &ReplayDock::onClipTableSelectionChanged);
	connect(m_tableClips, &QTableWidget::cellDoubleClicked, this, &ReplayDock::onClipDoubleClicked);

	mainLayout->addWidget(m_tableClips, 1);

	// -------------------------------------------------------------------------
	// 4. Panel de Etiquetado Deportivo Rápido
	// -------------------------------------------------------------------------
	QHBoxLayout *tagBar = new QHBoxLayout();
	tagBar->setSpacing(4);

	QLabel *lblTagTitle = new QLabel(QStringLiteral("Etiquetar Selección:"), mainWidget);
	m_btnTagHome = new QPushButton(QStringLiteral("[ C ] Casa"), mainWidget);
	m_btnTagAway = new QPushButton(QStringLiteral("[ V ] Visitante"), mainWidget);
	m_btnTag3PT = new QPushButton(QStringLiteral("[ 3PT ] Triple"), mainWidget);
	m_btnTagTop = new QPushButton(QStringLiteral("[ TOP ] Destacada"), mainWidget);
	m_btnTagVar = new QPushButton(QStringLiteral("[ VAR ] Árbitro"), mainWidget);
	m_btnTagClear = new QPushButton(QStringLiteral("Limpiar"), mainWidget);

	connect(m_btnTagHome, &QPushButton::clicked, this, &ReplayDock::onTagClicked);
	connect(m_btnTagAway, &QPushButton::clicked, this, &ReplayDock::onTagClicked);
	connect(m_btnTag3PT, &QPushButton::clicked, this, &ReplayDock::onTagClicked);
	connect(m_btnTagTop, &QPushButton::clicked, this, &ReplayDock::onTagClicked);
	connect(m_btnTagVar, &QPushButton::clicked, this, &ReplayDock::onTagClicked);
	connect(m_btnTagClear, &QPushButton::clicked, this, &ReplayDock::onTagClicked);

	tagBar->addWidget(lblTagTitle);
	tagBar->addWidget(m_btnTagHome);
	tagBar->addWidget(m_btnTagAway);
	tagBar->addSpacing(8);
	tagBar->addWidget(m_btnTag3PT);
	tagBar->addWidget(m_btnTagTop);
	tagBar->addWidget(m_btnTagVar);
	tagBar->addWidget(m_btnTagClear);
	tagBar->addStretch(1);

	mainLayout->addLayout(tagBar);

	// -------------------------------------------------------------------------
	// 5. Selector de Casilla (Double Deck A / B)
	// -------------------------------------------------------------------------
	QGroupBox *deckGroup = new QGroupBox(QStringLiteral("Control de Reproducción (Double Deck)"), mainWidget);
	QVBoxLayout *deckLayout = new QVBoxLayout(deckGroup);
	deckLayout->setContentsMargins(6, 6, 6, 6);
	deckLayout->setSpacing(5);

	QHBoxLayout *deckSelectorBar = new QHBoxLayout();
	m_btnSelectDeckA = new QPushButton(QStringLiteral("DECK A (REPLAY)"), deckGroup);
	m_btnSelectDeckA->setCheckable(true);
	m_btnSelectDeckA->setChecked(true);
	m_btnSelectDeckA->setStyleSheet(QStringLiteral("QPushButton:checked { background-color: #2e7d32; color: white; font-weight: bold; }"));

	m_lblDeckAStatus = new QLabel(QStringLiteral("[LISTO]"), deckGroup);
	m_lblDeckAStatus->setStyleSheet(QStringLiteral("font-weight: bold; color: #81c784;"));

	m_btnSelectDeckB = new QPushButton(QStringLiteral("DECK B (REPLAY 2)"), deckGroup);
	m_btnSelectDeckB->setCheckable(true);
	m_btnSelectDeckB->setStyleSheet(QStringLiteral("QPushButton:checked { background-color: #2e7d32; color: white; font-weight: bold; }"));

	m_lblDeckBStatus = new QLabel(QStringLiteral("[LISTO]"), deckGroup);
	m_lblDeckBStatus->setStyleSheet(QStringLiteral("font-weight: bold; color: #81c784;"));

	QButtonGroup *deckBtnGroup = new QButtonGroup(this);
	deckBtnGroup->addButton(m_btnSelectDeckA, 0);
	deckBtnGroup->addButton(m_btnSelectDeckB, 1);
	connect(deckBtnGroup, &QButtonGroup::idClicked, this, &ReplayDock::onDeckTabChanged);

	deckSelectorBar->addWidget(m_btnSelectDeckA);
	deckSelectorBar->addWidget(m_lblDeckAStatus);
	deckSelectorBar->addSpacing(20);
	deckSelectorBar->addWidget(m_btnSelectDeckB);
	deckSelectorBar->addWidget(m_lblDeckBStatus);
	deckSelectorBar->addStretch(1);

	deckLayout->addLayout(deckSelectorBar);

	// Multi-cámara instantánea (botones dinámicos)
	QHBoxLayout *angleContainer = new QHBoxLayout();
	angleContainer->addWidget(new QLabel(QStringLiteral("Ángulos en Vivo:"), deckGroup));
	m_angleLayout = new QHBoxLayout();
	angleContainer->addLayout(m_angleLayout);
	angleContainer->addStretch(1);
	deckLayout->addLayout(angleContainer);

	// Clip Title & Time
	QHBoxLayout *clipInfoBar = new QHBoxLayout();
	m_lblCurrentClipTitle = new QLabel(QStringLiteral("Sin clip cargado"), deckGroup);
	m_lblCurrentClipTitle->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 11pt;"));
	m_lblTime = new QLabel(QStringLiteral("00:00.000 / 00:00.000"), deckGroup);
	m_lblTime->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 11pt;"));
	clipInfoBar->addWidget(m_lblCurrentClipTitle, 1);
	clipInfoBar->addWidget(m_lblTime);
	deckLayout->addLayout(clipInfoBar);

	// Timeline Scrubbing Slider
	m_sliderTimeline = new QSlider(Qt::Horizontal, deckGroup);
	m_sliderTimeline->setRange(0, 1000);
	m_sliderTimeline->setValue(0);
	connect(m_sliderTimeline, &QSlider::sliderMoved, this, &ReplayDock::onTimelineSliderMoved);
	connect(m_sliderTimeline, &QSlider::sliderReleased, this, &ReplayDock::onTimelineSliderReleased);
	deckLayout->addWidget(m_sliderTimeline);

	// Transporte: Step -, Play/Pause, Step +, Stop + Take/Preview
	QHBoxLayout *transportBar = new QHBoxLayout();
	transportBar->setSpacing(4);

	m_btnStepBack = new QPushButton(QStringLiteral("[-1 Frame]"), deckGroup);
	m_btnStepBack->setToolTip(QStringLiteral("Retroceder exactamente un cuadro"));
	connect(m_btnStepBack, &QPushButton::clicked, this, &ReplayDock::onStepFrameBack);

	m_btnPlayPause = new QPushButton(QStringLiteral("PLAY"), deckGroup);
	m_btnPlayPause->setMinimumWidth(80);
	m_btnPlayPause->setStyleSheet(QStringLiteral("QPushButton { background-color: #1565c0; color: white; font-weight: bold; font-size: 10pt; } QPushButton:hover { background-color: #1e88e5; }"));
	connect(m_btnPlayPause, &QPushButton::clicked, this, &ReplayDock::onPlayPauseClicked);

	m_btnStepFwd = new QPushButton(QStringLiteral("[+1 Frame]"), deckGroup);
	m_btnStepFwd->setToolTip(QStringLiteral("Avanzar exactamente un cuadro"));
	connect(m_btnStepFwd, &QPushButton::clicked, this, &ReplayDock::onStepFrameFwd);

	m_btnStop = new QPushButton(QStringLiteral("STOP"), deckGroup);
	connect(m_btnStop, &QPushButton::clicked, this, &ReplayDock::onStopClicked);

	transportBar->addWidget(m_btnStepBack);
	transportBar->addWidget(m_btnPlayPause);
	transportBar->addWidget(m_btnStepFwd);
	transportBar->addWidget(m_btnStop);
	transportBar->addSpacing(15);

	// Velocidad
	QLabel *lblSpeed = new QLabel(QStringLiteral("Velocidad:"), deckGroup);
	m_sliderSpeed = new QSlider(Qt::Horizontal, deckGroup);
	m_sliderSpeed->setRange(0, 100);
	m_sliderSpeed->setValue(100);
	m_sliderSpeed->setMaximumWidth(120);
	connect(m_sliderSpeed, &QSlider::valueChanged, this, &ReplayDock::onSpeedSliderChanged);

	m_lblSpeedVal = new QLabel(QStringLiteral("100%"), deckGroup);
	m_lblSpeedVal->setMinimumWidth(35);

	m_btnSpeed25 = new QPushButton(QStringLiteral("25%"), deckGroup);
	m_btnSpeed50 = new QPushButton(QStringLiteral("50%"), deckGroup);
	m_btnSpeed75 = new QPushButton(QStringLiteral("75%"), deckGroup);
	m_btnSpeed100 = new QPushButton(QStringLiteral("100%"), deckGroup);

	connect(m_btnSpeed25, &QPushButton::clicked, this, [this]() { onSpeedQuickBtn(25); });
	connect(m_btnSpeed50, &QPushButton::clicked, this, [this]() { onSpeedQuickBtn(50); });
	connect(m_btnSpeed75, &QPushButton::clicked, this, [this]() { onSpeedQuickBtn(75); });
	connect(m_btnSpeed100, &QPushButton::clicked, this, [this]() { onSpeedQuickBtn(100); });

	transportBar->addWidget(lblSpeed);
	transportBar->addWidget(m_sliderSpeed);
	transportBar->addWidget(m_lblSpeedVal);
	transportBar->addWidget(m_btnSpeed25);
	transportBar->addWidget(m_btnSpeed50);
	transportBar->addWidget(m_btnSpeed75);
	transportBar->addWidget(m_btnSpeed100);
	transportBar->addStretch(1);

	// Acciones de Emisión
	m_btnSendPreview = new QPushButton(QStringLiteral("VER EN PREVIEW"), deckGroup);
	m_btnSendPreview->setStyleSheet(QStringLiteral("QPushButton { background-color: #e65100; color: white; font-weight: bold; } QPushButton:hover { background-color: #f57c00; }"));
	connect(m_btnSendPreview, &QPushButton::clicked, this, &ReplayDock::onSendPreviewClicked);

	m_btnTakeProgram = new QPushButton(QStringLiteral("ENVIAR A PROGRAMA (TAKE)"), deckGroup);
	m_btnTakeProgram->setStyleSheet(QStringLiteral("QPushButton { background-color: #2e7d32; color: white; font-weight: bold; padding: 5px 12px; } QPushButton:hover { background-color: #388e3c; }"));
	connect(m_btnTakeProgram, &QPushButton::clicked, this, &ReplayDock::onTakeProgramClicked);

	transportBar->addWidget(m_btnSendPreview);
	transportBar->addWidget(m_btnTakeProgram);

	deckLayout->addLayout(transportBar);
	mainLayout->addWidget(deckGroup);

	// -------------------------------------------------------------------------
	// 6. Barra de Estado
	// -------------------------------------------------------------------------
	m_lblStatus = new QLabel(QStringLiteral("Plugin listo."), mainWidget);
	m_lblStatus->setStyleSheet(QStringLiteral("color: #9e9e9e; font-size: 9pt;"));
	mainLayout->addWidget(m_lblStatus);

	setWidget(mainWidget);
}

void ReplayDock::setupStyles()
{
	// Ajustes menores complementarios sobre los estilos nativos de OBS Studio
	setStyleSheet(QStringLiteral(
		"QTableWidget { gridline-color: #424242; font-size: 10pt; }"
		"QTableWidget::item:selected { background-color: #1976d2; color: white; }"
		"QHeaderView::section { background-color: #2b2b2b; color: #eeeeee; padding: 4px; border: 1px solid #3c3c3c; }"
	));
}

// ===========================================================================
// Configuración y Escenas
// ===========================================================================
void ReplayDock::loadSettings()
{
	config_t *cfg = obs_frontend_get_global_config();
	if (!cfg)
		return;

	const char *folder = config_get_string(cfg, kConfigSection, "ReplayFolder");
	if (folder && *folder) {
		m_txtFolder->setText(QString::fromUtf8(folder));
		m_tagStore.load(m_txtFolder->text());
		m_scanner.setFolder(m_txtFolder->text());
	}

	const char *liveScene = config_get_string(cfg, kConfigSection, "LiveScene");
	if (liveScene && *liveScene) {
		m_engine.setLiveScene(QString::fromUtf8(liveScene));
	}
}

void ReplayDock::saveSettings()
{
	config_t *cfg = obs_frontend_get_global_config();
	if (!cfg)
		return;

	config_set_string(cfg, kConfigSection, "ReplayFolder", m_txtFolder->text().toUtf8().constData());
	config_set_string(cfg, kConfigSection, "LiveScene", m_engine.liveScene().toUtf8().constData());
	config_save(cfg);
}

void ReplayDock::refreshSceneList()
{
	char **scenes = obs_frontend_get_scene_names();
	if (!scenes)
		return;

	m_comboLiveScene->blockSignals(true);
	m_comboLiveScene->clear();

	int selectIndex = -1;
	const QString targetLive = m_engine.liveScene();

	for (size_t i = 0; scenes[i] != nullptr; ++i) {
		const QString sName = QString::fromUtf8(scenes[i]);
		// Omitir las escenas de replay del selector de cámara en vivo
		if (sName == QString::fromUtf8(kDeckSceneNames[0]) || sName == QString::fromUtf8(kDeckSceneNames[1]))
			continue;

		m_comboLiveScene->addItem(sName);
		if (sName.compare(targetLive, Qt::CaseInsensitive) == 0)
			selectIndex = m_comboLiveScene->count() - 1;
	}
	bfree(scenes);

	if (selectIndex >= 0)
		m_comboLiveScene->setCurrentIndex(selectIndex);
	else if (m_comboLiveScene->count() > 0)
		m_comboLiveScene->setCurrentIndex(0);

	m_comboLiveScene->blockSignals(false);
	if (m_comboLiveScene->currentIndex() >= 0)
		m_engine.setLiveScene(m_comboLiveScene->currentText());
}

// ===========================================================================
// Manejo de Clips y Tabla
// ===========================================================================
void ReplayDock::onBrowseFolder()
{
	const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Seleccionar Carpeta de Repeticiones"),
							      m_txtFolder->text());
	if (dir.isEmpty())
		return;

	m_txtFolder->setText(dir);
	m_tagStore.load(dir);
	m_scanner.setFolder(dir);
	saveSettings();
}

void ReplayDock::onClipsUpdated(const bbr::ClipList &clips)
{
	m_allClips = clips;
	populateClipTable();
	updateAngleButtons(m_activeDeckTab);
}

void ReplayDock::onFilterChanged()
{
	populateClipTable();
}

void ReplayDock::populateClipTable()
{
	const int camFilter = m_comboCameraFilter->currentData().toInt();
	const int teamFilter = m_filterTeamGroup->checkedId();
	const bool filter3PT = m_btnFilter3PT->isChecked();
	const bool filterTop = m_btnFilterTop->isChecked();
	const bool filterVar = m_btnFilterVar->isChecked();

	m_visibleClips.clear();
	for (const ClipInfo &c : m_allClips) {
		if (camFilter >= 0 && c.camera != camFilter)
			continue;

		const ClipTags tags = m_tagStore.get(c.fileName);
		if (teamFilter == 1 && tags.team != Team::Home)
			continue;
		if (teamFilter == 2 && tags.team != Team::Away)
			continue;
		if (filter3PT && !tags.has(Play3PT))
			continue;
		if (filterTop && !tags.has(PlayTop))
			continue;
		if (filterVar && !tags.has(PlayVar))
			continue;

		m_visibleClips.push_back(c);
	}

	m_tableClips->setRowCount(m_visibleClips.size());
	for (int r = 0; r < m_visibleClips.size(); ++r) {
		const ClipInfo &c = m_visibleClips[r];
		const ClipTags tags = m_tagStore.get(c.fileName);

		QTableWidgetItem *itemNum = new QTableWidgetItem(QStringLiteral("%1").arg(c.number, 3, 10, QLatin1Char('0')));
		itemNum->setTextAlignment(Qt::AlignCenter);

		const QString timeStr = QDateTime::fromMSecsSinceEpoch(c.mtimeMs).toString(QStringLiteral("HH:mm:ss"));
		QTableWidgetItem *itemName = new QTableWidgetItem(QStringLiteral("%1  (%2)").arg(c.fileName, timeStr));

		QTableWidgetItem *itemCam = new QTableWidgetItem(c.cameraLongLabel());
		itemCam->setTextAlignment(Qt::AlignCenter);

		QTableWidgetItem *itemBadges = new QTableWidgetItem(tags.badges());
		itemBadges->setTextAlignment(Qt::AlignCenter);
		if (tags.has(PlayVar))
			itemBadges->setForeground(QBrush(QColor(255, 82, 82)));
		else if (tags.has(PlayTop))
			itemBadges->setForeground(QBrush(QColor(255, 215, 0)));
		else if (tags.has(Play3PT))
			itemBadges->setForeground(QBrush(QColor(76, 175, 80)));

		m_tableClips->setItem(r, 0, itemNum);
		m_tableClips->setItem(r, 1, itemName);
		m_tableClips->setItem(r, 2, itemCam);
		m_tableClips->setItem(r, 3, itemBadges);
	}
}

void ReplayDock::onClipTableSelectionChanged()
{
	// Actualizar botones de marcado o resaltado según sea necesario
}

void ReplayDock::onClipDoubleClicked(int row, int /*col*/)
{
	if (row < 0 || row >= m_visibleClips.size())
		return;

	// Si hay varios clips seleccionados, cargamos en modo Playlist continua
	const QModelIndexList selRows = m_tableClips->selectionModel()->selectedRows();
	if (selRows.size() > 1) {
		ClipList playlist;
		for (const QModelIndex &idx : selRows) {
			const int r = idx.row();
			if (r >= 0 && r < m_visibleClips.size())
				playlist.push_back(m_visibleClips[r]);
		}
		const int targetDeck = m_engine.pickFreeDeck(m_activeDeckTab);
		m_engine.loadPlaylist(targetDeck, playlist);
		m_activeDeckTab = targetDeck;
		m_btnSelectDeckA->setChecked(m_activeDeckTab == 0);
		m_btnSelectDeckB->setChecked(m_activeDeckTab == 1);
		updateTransportUi(m_activeDeckTab);
		return;
	}

	const ClipInfo &clip = m_visibleClips[row];
	const int targetDeck = m_engine.pickFreeDeck(m_activeDeckTab);
	m_engine.loadClip(targetDeck, clip, false);
	m_activeDeckTab = targetDeck;
	m_btnSelectDeckA->setChecked(m_activeDeckTab == 0);
	m_btnSelectDeckB->setChecked(m_activeDeckTab == 1);
	updateTransportUi(m_activeDeckTab);
}

// ===========================================================================
// Etiquetas Deportivas
// ===========================================================================
void ReplayDock::onTagClicked()
{
	QPushButton *senderBtn = qobject_cast<QPushButton *>(sender());
	if (!senderBtn)
		return;

	const QModelIndexList selRows = m_tableClips->selectionModel()->selectedRows();
	if (selRows.isEmpty())
		return;

	for (const QModelIndex &idx : selRows) {
		const int r = idx.row();
		if (r < 0 || r >= m_visibleClips.size())
			continue;

		const QString fName = m_visibleClips[r].fileName;
		ClipTags t = m_tagStore.get(fName);

		if (senderBtn == m_btnTagHome) {
			t.team = (t.team == Team::Home) ? Team::None : Team::Home;
		} else if (senderBtn == m_btnTagAway) {
			t.team = (t.team == Team::Away) ? Team::None : Team::Away;
		} else if (senderBtn == m_btnTag3PT) {
			t.set(Play3PT, !t.has(Play3PT));
		} else if (senderBtn == m_btnTagTop) {
			t.set(PlayTop, !t.has(PlayTop));
		} else if (senderBtn == m_btnTagVar) {
			t.set(PlayVar, !t.has(PlayVar));
		} else if (senderBtn == m_btnTagClear) {
			t = ClipTags();
		}

		m_tagStore.set(fName, t);
	}

	m_tagStore.save();
	populateClipTable();
}

void ReplayDock::onHalftimeSummaryClicked()
{
	ClipList summaryClips;
	for (const ClipInfo &c : m_allClips) {
		const ClipTags tags = m_tagStore.get(c.fileName);
		if (tags.has(Play3PT) || tags.has(PlayTop))
			summaryClips.push_back(c);
	}

	if (summaryClips.isEmpty()) {
		m_lblStatus->setText(QStringLiteral("No hay clips marcados como 3PT o TOP para el resumen."));
		return;
	}

	// Ordenar cronológicamente
	std::sort(summaryClips.begin(), summaryClips.end(), [](const ClipInfo &a, const ClipInfo &b) {
		return a.mtimeMs < b.mtimeMs;
	});

	const int targetDeck = m_engine.pickFreeDeck(m_activeDeckTab);
	m_engine.loadPlaylist(targetDeck, summaryClips);
	m_activeDeckTab = targetDeck;
	m_btnSelectDeckA->setChecked(m_activeDeckTab == 0);
	m_btnSelectDeckB->setChecked(m_activeDeckTab == 1);
	updateTransportUi(m_activeDeckTab);
	m_lblStatus->setText(QStringLiteral("Resumen cargado: %1 jugadas destacadas.").arg(summaryClips.size()));
}

// ===========================================================================
// Control de Deck y Transporte
// ===========================================================================
void ReplayDock::onDeckTabChanged(int index)
{
	m_activeDeckTab = index;
	updateTransportUi(m_activeDeckTab);
	updateAngleButtons(m_activeDeckTab);
}

void ReplayDock::onPlayPauseClicked()
{
	m_engine.togglePlayPause(m_activeDeckTab);
	updateTransportUi(m_activeDeckTab);
}

void ReplayDock::onStopClicked()
{
	m_engine.stop(m_activeDeckTab);
	updateTransportUi(m_activeDeckTab);
}

void ReplayDock::onStepFrameBack()
{
	m_engine.stepFrame(m_activeDeckTab, -1);
}

void ReplayDock::onStepFrameFwd()
{
	m_engine.stepFrame(m_activeDeckTab, 1);
}

void ReplayDock::onSpeedSliderChanged(int value)
{
	m_lblSpeedVal->setText(QStringLiteral("%1%").arg(value));
	m_engine.setSpeed(m_activeDeckTab, value, false);
}

void ReplayDock::onSpeedQuickBtn(int percent)
{
	m_sliderSpeed->setValue(percent);
	m_lblSpeedVal->setText(QStringLiteral("%1%").arg(percent));
	m_engine.setSpeed(m_activeDeckTab, percent, true);
}

void ReplayDock::onTimelineSliderMoved(int position)
{
	m_isScrubbing = true;
	const qint64 dur = m_engine.mediaDuration(m_activeDeckTab);
	if (dur > 0) {
		const qint64 targetMs = (position * dur) / 1000;
		m_lblTime->setText(QStringLiteral("%1 / %2").arg(formatTimeMs(targetMs), formatTimeMs(dur)));
		m_engine.seek(m_activeDeckTab, targetMs);
	}
}

void ReplayDock::onTimelineSliderReleased()
{
	m_isScrubbing = false;
}

// ===========================================================================
// Transición y Emisión
// ===========================================================================
void ReplayDock::onSendPreviewClicked()
{
	m_engine.sendToPreview(m_activeDeckTab);
}

void ReplayDock::onTakeProgramClicked()
{
	m_engine.takeToProgram(m_activeDeckTab);
}

void ReplayDock::onReturnLiveClicked()
{
	m_engine.returnToLive();
}

void ReplayDock::onLiveSceneComboChanged(int index)
{
	if (index >= 0)
		m_engine.setLiveScene(m_comboLiveScene->currentText());
}

// ===========================================================================
// Multi-cámara instantánea
// ===========================================================================
void ReplayDock::updateAngleButtons(int deck)
{
	// Limpiar botones anteriores
	qDeleteAll(m_angleButtons);
	m_angleButtons.clear();

	const ClipInfo current = m_engine.currentClip(deck);
	if (!current.isValid() || current.groupKey.isEmpty())
		return;

	// Buscar todos los clips que pertenecen al mismo grupo de grabación
	for (const ClipInfo &c : m_allClips) {
		if (c.groupKey == current.groupKey) {
			QPushButton *btn = new QPushButton(c.cameraLongLabel(), this);
			btn->setCheckable(true);
			btn->setChecked(c.camera == current.camera);
			if (btn->isChecked())
				btn->setStyleSheet(QStringLiteral("font-weight: bold; background-color: #1565c0; color: white;"));

			connect(btn, &QPushButton::clicked, this, [this, c]() {
				m_engine.switchAngle(m_activeDeckTab, c);
				updateAngleButtons(m_activeDeckTab);
			});

			m_angleLayout->addWidget(btn);
			m_angleButtons.push_back(btn);
		}
	}
}

void ReplayDock::onCameraSwitchClicked(int /*camIndex*/)
{
}

// ===========================================================================
// Actualización Periódica y de Motor
// ===========================================================================
void ReplayDock::onEngineDeckChanged(int deck)
{
	updateDeckStatusBadges();
	if (deck == m_activeDeckTab) {
		updateTransportUi(deck);
		updateAngleButtons(deck);
	}
}

void ReplayDock::onEngineSceneStateChanged()
{
	refreshSceneList();
	updateDeckStatusBadges();
}

void ReplayDock::onEngineStatusMessage(const QString &msg)
{
	m_lblStatus->setText(msg);
}

void ReplayDock::updateDeckStatusBadges()
{
	const auto formatBadge = [this](int deck) -> QString {
		const bool onAir = m_engine.isOnAir(deck);
		const bool inPrev = m_engine.isInPreview(deck);
		const ClipInfo c = m_engine.currentClip(deck);

		if (onAir)
			return QStringLiteral("<font color='#f44336'>[AL AIRE]</font>");
		if (inPrev)
			return QStringLiteral("<font color='#ff9800'>[PREVIEW]</font>");
		if (c.isValid())
			return QStringLiteral("<font color='#81c784'>[LISTO]</font>");
		return QStringLiteral("<font color='#9e9e9e'>[VACÍO]</font>");
	};

	m_lblDeckAStatus->setText(formatBadge(0));
	m_lblDeckBStatus->setText(formatBadge(1));
}

void ReplayDock::updateTransportUi(int deck)
{
	const ClipInfo c = m_engine.currentClip(deck);
	const int pIndex = m_engine.playlistIndex(deck);
	const int pTotal = m_engine.playlistSize(deck);

	if (c.isValid()) {
		if (pTotal > 1) {
			m_lblCurrentClipTitle->setText(
				QStringLiteral("[%1] Playlist (%2/%3): %4")
					.arg(QString::fromUtf8(kDeckLabels[deck]))
					.arg(pIndex + 1)
					.arg(pTotal)
					.arg(c.displayName()));
		} else {
			m_lblCurrentClipTitle->setText(
				QStringLiteral("[%1] %2").arg(QString::fromUtf8(kDeckLabels[deck]), c.displayName()));
		}
	} else {
		m_lblCurrentClipTitle->setText(QStringLiteral("[%1] Sin clip cargado").arg(QString::fromUtf8(kDeckLabels[deck])));
	}

	const int spd = m_engine.speed(deck);
	m_sliderSpeed->blockSignals(true);
	m_sliderSpeed->setValue(spd);
	m_lblSpeedVal->setText(QStringLiteral("%1%").arg(spd));
	m_sliderSpeed->blockSignals(false);

	const obs_media_state st = m_engine.mediaState(deck);
	const bool isPlaying = (st == OBS_MEDIA_STATE_PLAYING);
	m_btnPlayPause->setText(isPlaying ? QStringLiteral("PAUSA") : QStringLiteral("PLAY"));
	m_btnPlayPause->setStyleSheet(isPlaying
		? QStringLiteral("QPushButton { background-color: #f57f17; color: white; font-weight: bold; }")
		: QStringLiteral("QPushButton { background-color: #1565c0; color: white; font-weight: bold; }"));
}

void ReplayDock::onUiTick()
{
	const qint64 t = m_engine.mediaTime(m_activeDeckTab);
	const qint64 dur = m_engine.mediaDuration(m_activeDeckTab);

	if (!m_isScrubbing) {
		m_lblTime->setText(QStringLiteral("%1 / %2").arg(formatTimeMs(t), formatTimeMs(dur)));
		if (dur > 0) {
			m_sliderTimeline->blockSignals(true);
			m_sliderTimeline->setValue(int((t * 1000) / dur));
			m_sliderTimeline->blockSignals(false);
		} else {
			m_sliderTimeline->setValue(0);
		}
	}
}

} // namespace bbr
