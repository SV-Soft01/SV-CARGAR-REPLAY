#include "replay-dock.hpp"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QMenu>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>

#include <algorithm>
#include <cmath>
#include <cstring>

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
	QVBoxLayout *rootLayout = new QVBoxLayout(mainWidget);
	rootLayout->setContentsMargins(3, 3, 3, 3);
	rootLayout->setSpacing(3);

	// -------------------------------------------------------------------------
	// 1. Barra Superior Principal: Carpeta, En Vivo y Cambio de Orientación
	// -------------------------------------------------------------------------
	QHBoxLayout *topBar = new QHBoxLayout();
	topBar->setSpacing(3);

	QLabel *lblFolder = new QLabel(QStringLiteral("Carpeta:"), mainWidget);
	m_txtFolder = new QLineEdit(mainWidget);
	m_txtFolder->setPlaceholderText(QStringLiteral("Ruta a grabaciones de repetición..."));
	m_txtFolder->setReadOnly(true);

	m_btnBrowse = new QPushButton(QStringLiteral("Examinar..."), mainWidget);
	connect(m_btnBrowse, &QPushButton::clicked, this, &ReplayDock::onBrowseFolder);

	QLabel *lblLive = new QLabel(QStringLiteral("En Vivo:"), mainWidget);
	m_comboLiveScene = new QComboBox(mainWidget);
	connect(m_comboLiveScene, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&ReplayDock::onLiveSceneComboChanged);

	m_btnReturnLive = new QPushButton(QStringLiteral("VOLVER A VIVO"), mainWidget);
	m_btnReturnLive->setObjectName(QStringLiteral("btnReturnLive"));
	m_btnReturnLive->setStyleSheet(QStringLiteral("QPushButton { background-color: #b71c1c; color: white; font-weight: bold; padding: 3px 8px; border-radius: 3px; } QPushButton:hover { background-color: #d32f2f; }"));
	connect(m_btnReturnLive, &QPushButton::clicked, this, &ReplayDock::onReturnLiveClicked);

	m_btnToggleOrientation = new QPushButton(m_isHorizontalLayout ? QStringLiteral("⬌ Lado a Lado") : QStringLiteral("⬍ Apilado"), mainWidget);
	m_btnToggleOrientation->setToolTip(QStringLiteral("Cambiar disposición: Lado a Lado (videos a la izquierda, controles a la derecha) o Apilado (arriba/abajo)"));
	m_btnToggleOrientation->setStyleSheet(QStringLiteral("QPushButton { background-color: #37474f; color: white; font-weight: bold; }"));
	connect(m_btnToggleOrientation, &QPushButton::clicked, this, &ReplayDock::onToggleOrientation);

	topBar->addWidget(lblFolder);
	topBar->addWidget(m_txtFolder, 1);
	topBar->addWidget(m_btnBrowse);
	topBar->addSpacing(8);
	topBar->addWidget(lblLive);
	topBar->addWidget(m_comboLiveScene);
	topBar->addWidget(m_btnReturnLive);
	topBar->addSpacing(6);
	topBar->addWidget(m_btnToggleOrientation);

	rootLayout->addLayout(topBar);

	// -------------------------------------------------------------------------
	// 2. QSplitter Adaptable: Panel de Videos y Panel de Controles
	// -------------------------------------------------------------------------
	m_mainSplitter = new QSplitter(m_isHorizontalLayout ? Qt::Horizontal : Qt::Vertical, mainWidget);
	m_mainSplitter->setChildrenCollapsible(false);

	// =========================================================================
	// PANEL A: Lista de Videos y Filtros
	// =========================================================================
	QWidget *videoPanel = new QWidget(m_mainSplitter);
	QVBoxLayout *videoLayout = new QVBoxLayout(videoPanel);
	videoLayout->setContentsMargins(1, 1, 1, 1);
	videoLayout->setSpacing(2);

	// Filtros de Cámara y Etiquetas Deportivas
	QHBoxLayout *filterBar = new QHBoxLayout();
	filterBar->setSpacing(2);

	m_comboCameraFilter = new QComboBox(videoPanel);
	m_comboCameraFilter->addItem(QStringLiteral("Todas las cámaras"), QString());
	connect(m_comboCameraFilter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&ReplayDock::onFilterChanged);

	m_filterTeamGroup = new QButtonGroup(this);
	m_btnFilterAll = new QPushButton(QStringLiteral("Todos"), videoPanel);
	m_btnFilterAll->setCheckable(true);
	m_btnFilterAll->setChecked(true);
	m_btnFilterHome = new QPushButton(QStringLiteral("Casa [C]"), videoPanel);
	m_btnFilterHome->setCheckable(true);
	m_btnFilterAway = new QPushButton(QStringLiteral("Vis [V]"), videoPanel);
	m_btnFilterAway->setCheckable(true);
	m_filterTeamGroup->addButton(m_btnFilterAll, 0);
	m_filterTeamGroup->addButton(m_btnFilterHome, 1);
	m_filterTeamGroup->addButton(m_btnFilterAway, 2);
	connect(m_filterTeamGroup, &QButtonGroup::idClicked, this, &ReplayDock::onFilterChanged);

	m_btnFilter3PT = new QPushButton(QStringLiteral("3PT"), videoPanel);
	m_btnFilter3PT->setCheckable(true);
	connect(m_btnFilter3PT, &QPushButton::toggled, this, &ReplayDock::onFilterChanged);

	m_btnFilterTop = new QPushButton(QStringLiteral("TOP"), videoPanel);
	m_btnFilterTop->setCheckable(true);
	connect(m_btnFilterTop, &QPushButton::toggled, this, &ReplayDock::onFilterChanged);

	m_btnFilterVar = new QPushButton(QStringLiteral("VAR"), videoPanel);
	m_btnFilterVar->setCheckable(true);
	connect(m_btnFilterVar, &QPushButton::toggled, this, &ReplayDock::onFilterChanged);

	filterBar->addWidget(m_comboCameraFilter);
	filterBar->addWidget(m_btnFilterAll);
	filterBar->addWidget(m_btnFilterHome);
	filterBar->addWidget(m_btnFilterAway);
	filterBar->addWidget(m_btnFilter3PT);
	filterBar->addWidget(m_btnFilterTop);
	filterBar->addWidget(m_btnFilterVar);
	filterBar->addStretch(1);

	videoLayout->addLayout(filterBar);

	// Tabla Central de Clips (ancho ajustado sin espacios negros vacíos)
	m_tableClips = new QTableWidget(videoPanel);
	m_tableClips->setColumnCount(4);
	m_tableClips->setHorizontalHeaderLabels({QStringLiteral("Nº"), QStringLiteral("Hora / Clip"), QStringLiteral("Cámara"), QStringLiteral("Etiquetas")});
	
	m_tableClips->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
	m_tableClips->horizontalHeader()->resizeSection(0, 36);
	m_tableClips->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
	m_tableClips->horizontalHeader()->resizeSection(1, 230);
	m_tableClips->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
	m_tableClips->horizontalHeader()->resizeSection(2, 105);
	m_tableClips->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
	m_tableClips->horizontalHeader()->resizeSection(3, 75);
	m_tableClips->horizontalHeader()->setStretchLastSection(false);

	m_tableClips->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_tableClips->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_tableClips->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_tableClips->setAlternatingRowColors(true);
	m_tableClips->setMinimumWidth(260);
	m_tableClips->setMinimumHeight(60);
	m_tableClips->setContextMenuPolicy(Qt::CustomContextMenu);

	connect(m_tableClips, &QTableWidget::cellClicked, this, &ReplayDock::onClipClicked);
	connect(m_tableClips, &QTableWidget::cellDoubleClicked, this, &ReplayDock::onClipDoubleClicked);
	connect(m_tableClips, &QTableWidget::customContextMenuRequested, this, &ReplayDock::onClipContextMenu);

	videoLayout->addWidget(m_tableClips, 1);

	// Botones rápidos de etiquetado bajo la tabla
	QHBoxLayout *tagBar = new QHBoxLayout();
	tagBar->setSpacing(2);
	QLabel *lblTagTitle = new QLabel(QStringLiteral("Marcar:"), videoPanel);
	m_btnTagHome = new QPushButton(QStringLiteral("[C] Casa"), videoPanel);
	m_btnTagAway = new QPushButton(QStringLiteral("[V] Vis"), videoPanel);
	m_btnTag3PT = new QPushButton(QStringLiteral("[3PT]"), videoPanel);
	m_btnTagTop = new QPushButton(QStringLiteral("[TOP]"), videoPanel);
	m_btnTagVar = new QPushButton(QStringLiteral("[VAR]"), videoPanel);
	m_btnTagClear = new QPushButton(QStringLiteral("Limpiar"), videoPanel);

	connect(m_btnTagHome, &QPushButton::clicked, this, &ReplayDock::onTagClicked);
	connect(m_btnTagAway, &QPushButton::clicked, this, &ReplayDock::onTagClicked);
	connect(m_btnTag3PT, &QPushButton::clicked, this, &ReplayDock::onTagClicked);
	connect(m_btnTagTop, &QPushButton::clicked, this, &ReplayDock::onTagClicked);
	connect(m_btnTagVar, &QPushButton::clicked, this, &ReplayDock::onTagClicked);
	connect(m_btnTagClear, &QPushButton::clicked, this, &ReplayDock::onTagClicked);

	tagBar->addWidget(lblTagTitle);
	tagBar->addWidget(m_btnTagHome);
	tagBar->addWidget(m_btnTagAway);
	tagBar->addWidget(m_btnTag3PT);
	tagBar->addWidget(m_btnTagTop);
	tagBar->addWidget(m_btnTagVar);
	tagBar->addWidget(m_btnTagClear);
	tagBar->addStretch(1);

	videoLayout->addLayout(tagBar);

	// =========================================================================
	// PANEL B: Controles de Emisión, Doble Deck y Playlists
	// =========================================================================
	QWidget *controlPanel = new QWidget(m_mainSplitter);
	QVBoxLayout *controlLayout = new QVBoxLayout(controlPanel);
	controlLayout->setContentsMargins(1, 1, 1, 1);
	controlLayout->setSpacing(2);

	// Barra de Playlist Personalizada
	QHBoxLayout *plBar = new QHBoxLayout();
	plBar->setSpacing(3);

	m_btnAddToPlaylist = new QPushButton(QStringLiteral("+ Añadir a Playlist"), controlPanel);
	m_btnAddToPlaylist->setToolTip(QStringLiteral("Añadir jugadas seleccionadas a mi lista personalizada"));
	m_btnAddToPlaylist->setStyleSheet(QStringLiteral("QPushButton { background-color: #2e7d32; color: white; font-weight: bold; }"));
	connect(m_btnAddToPlaylist, &QPushButton::clicked, this, &ReplayDock::onAddToCustomPlaylist);

	m_btnFilterPlaylist = new QPushButton(QStringLiteral("★ Mi Playlist (0)"), controlPanel);
	m_btnFilterPlaylist->setToolTip(QStringLiteral("Filtrar para ver solo las jugadas de mi lista"));
	connect(m_btnFilterPlaylist, &QPushButton::clicked, this, &ReplayDock::onToggleCustomPlaylistFilter);

	m_btnLoadPlaylist = new QPushButton(QStringLiteral("▶ Cargar Playlist"), controlPanel);
	m_btnLoadPlaylist->setToolTip(QStringLiteral("Cargar toda mi playlist continua para emitir en secuencia"));
	m_btnLoadPlaylist->setStyleSheet(QStringLiteral("QPushButton { background-color: #0277bd; color: white; font-weight: bold; }"));
	connect(m_btnLoadPlaylist, &QPushButton::clicked, this, &ReplayDock::onLoadCustomPlaylist);

	m_btnClearPlaylist = new QPushButton(QStringLiteral("Limpiar PL"), controlPanel);
	connect(m_btnClearPlaylist, &QPushButton::clicked, this, &ReplayDock::onClearCustomPlaylist);

	m_btnHalftimeSummary = new QPushButton(QStringLiteral("Resumen Medio Tiempo"), controlPanel);
	m_btnHalftimeSummary->setStyleSheet(QStringLiteral("QPushButton { background-color: #4527a0; color: white; font-weight: bold; }"));
	connect(m_btnHalftimeSummary, &QPushButton::clicked, this, &ReplayDock::onHalftimeSummaryClicked);

	plBar->addWidget(m_btnAddToPlaylist);
	plBar->addWidget(m_btnFilterPlaylist);
	plBar->addWidget(m_btnLoadPlaylist);
	plBar->addWidget(m_btnClearPlaylist);
	plBar->addStretch(1);
	plBar->addWidget(m_btnHalftimeSummary);

	controlLayout->addLayout(plBar);

	// Grupo de Control de Decks y Reproducción
	QGroupBox *deckGroup = new QGroupBox(QStringLiteral("Control de Emisión (Double Deck A/B)"), controlPanel);
	QVBoxLayout *deckLayout = new QVBoxLayout(deckGroup);
	deckLayout->setContentsMargins(4, 4, 4, 4);
	deckLayout->setSpacing(3);

	// Pestañas de Deck y Estado
	QHBoxLayout *deckRow = new QHBoxLayout();
	deckRow->setSpacing(6);

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

	deckRow->addWidget(m_btnSelectDeckA);
	deckRow->addWidget(m_lblDeckAStatus);
	deckRow->addSpacing(12);
	deckRow->addWidget(m_btnSelectDeckB);
	deckRow->addWidget(m_lblDeckBStatus);
	deckRow->addStretch(1);

	deckLayout->addLayout(deckRow);

	// Ángulos de Cámara Sincronizados
	QHBoxLayout *angleRow = new QHBoxLayout();
	angleRow->setSpacing(4);
	QLabel *lblAnglesTitle = new QLabel(QStringLiteral("Ángulos en Vivo:"), deckGroup);
	angleRow->addWidget(lblAnglesTitle);
	m_angleLayout = new QHBoxLayout();
	m_angleLayout->setSpacing(3);
	angleRow->addLayout(m_angleLayout);
	angleRow->addStretch(1);

	deckLayout->addLayout(angleRow);

	// Título del clip actual y reloj
	QHBoxLayout *clipInfoBar = new QHBoxLayout();
	clipInfoBar->setSpacing(6);
	m_lblCurrentClipTitle = new QLabel(QStringLiteral("Sin clip cargado"), deckGroup);
	m_lblCurrentClipTitle->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 10pt;"));
	m_lblTime = new QLabel(QStringLiteral("00:00.000 / 00:00.000"), deckGroup);
	m_lblTime->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 10pt;"));
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

	// Transporte: Frame Back, Play, Frame Fwd, Stop
	QHBoxLayout *transportRow = new QHBoxLayout();
	transportRow->setSpacing(3);

	m_btnStepBack = new QPushButton(QStringLiteral("[-1 Frame]"), deckGroup);
	connect(m_btnStepBack, &QPushButton::clicked, this, &ReplayDock::onStepFrameBack);

	m_btnPlayPause = new QPushButton(QStringLiteral("PLAY"), deckGroup);
	m_btnPlayPause->setStyleSheet(QStringLiteral("QPushButton { background-color: #1565c0; color: white; font-weight: bold; min-width: 65px; } QPushButton:hover { background-color: #1e88e5; }"));
	connect(m_btnPlayPause, &QPushButton::clicked, this, &ReplayDock::onPlayPauseClicked);

	m_btnStepFwd = new QPushButton(QStringLiteral("[+1 Frame]"), deckGroup);
	connect(m_btnStepFwd, &QPushButton::clicked, this, &ReplayDock::onStepFrameFwd);

	m_btnStop = new QPushButton(QStringLiteral("STOP"), deckGroup);
	connect(m_btnStop, &QPushButton::clicked, this, &ReplayDock::onStopClicked);

	transportRow->addWidget(m_btnStepBack);
	transportRow->addWidget(m_btnPlayPause);
	transportRow->addWidget(m_btnStepFwd);
	transportRow->addWidget(m_btnStop);
	transportRow->addSpacing(8);

	// Control de Velocidad
	QLabel *lblSpeed = new QLabel(QStringLiteral("Vel:"), deckGroup);
	m_sliderSpeed = new QSlider(Qt::Horizontal, deckGroup);
	m_sliderSpeed->setRange(0, 100);
	m_sliderSpeed->setValue(100);
	connect(m_sliderSpeed, &QSlider::valueChanged, this, &ReplayDock::onSpeedSliderChanged);

	m_lblSpeedVal = new QLabel(QStringLiteral("100%"), deckGroup);

	m_btnSpeed25 = new QPushButton(QStringLiteral("25%"), deckGroup);
	m_btnSpeed50 = new QPushButton(QStringLiteral("50%"), deckGroup);
	m_btnSpeed75 = new QPushButton(QStringLiteral("75%"), deckGroup);
	m_btnSpeed100 = new QPushButton(QStringLiteral("100%"), deckGroup);

	connect(m_btnSpeed25, &QPushButton::clicked, this, [this]() { onSpeedQuickBtn(25); });
	connect(m_btnSpeed50, &QPushButton::clicked, this, [this]() { onSpeedQuickBtn(50); });
	connect(m_btnSpeed75, &QPushButton::clicked, this, [this]() { onSpeedQuickBtn(75); });
	connect(m_btnSpeed100, &QPushButton::clicked, this, [this]() { onSpeedQuickBtn(100); });

	transportRow->addWidget(lblSpeed);
	transportRow->addWidget(m_sliderSpeed, 1);
	transportRow->addWidget(m_lblSpeedVal);
	transportRow->addWidget(m_btnSpeed25);
	transportRow->addWidget(m_btnSpeed50);
	transportRow->addWidget(m_btnSpeed75);
	transportRow->addWidget(m_btnSpeed100);

	deckLayout->addLayout(transportRow);

	// Botones de Emisión (Preview y Take)
	QHBoxLayout *emissionRow = new QHBoxLayout();
	emissionRow->setSpacing(6);

	m_btnSendPreview = new QPushButton(QStringLiteral("VER EN PREVIEW"), deckGroup);
	m_btnSendPreview->setStyleSheet(QStringLiteral("QPushButton { background-color: #e65100; color: white; font-weight: bold; padding: 4px 10px; } QPushButton:hover { background-color: #f57c00; }"));
	connect(m_btnSendPreview, &QPushButton::clicked, this, &ReplayDock::onSendPreviewClicked);

	m_btnTakeProgram = new QPushButton(QStringLiteral("ENVIAR A PROGRAMA (TAKE)"), deckGroup);
	m_btnTakeProgram->setStyleSheet(QStringLiteral("QPushButton { background-color: #2e7d32; color: white; font-weight: bold; padding: 4px 12px; } QPushButton:hover { background-color: #388e3c; }"));
	connect(m_btnTakeProgram, &QPushButton::clicked, this, &ReplayDock::onTakeProgramClicked);

	emissionRow->addWidget(m_btnSendPreview, 1);
	emissionRow->addWidget(m_btnTakeProgram, 1);

	deckLayout->addLayout(emissionRow);
	controlLayout->addWidget(deckGroup);

	// Barra de Estado
	m_lblStatus = new QLabel(QStringLiteral("Plugin listo."), controlPanel);
	m_lblStatus->setStyleSheet(QStringLiteral("color: #9e9e9e; font-size: 8.5pt;"));
	controlLayout->addWidget(m_lblStatus);

	// Agregar paneles al Splitter
	m_mainSplitter->addWidget(videoPanel);
	m_mainSplitter->addWidget(controlPanel);

	// Proporción inicial equilibrada
	m_mainSplitter->setStretchFactor(0, 1);
	m_mainSplitter->setStretchFactor(1, 1);

	rootLayout->addWidget(m_mainSplitter, 1);
	setWidget(mainWidget);
}

void ReplayDock::onToggleOrientation()
{
	m_isHorizontalLayout = !m_isHorizontalLayout;
	m_mainSplitter->setOrientation(m_isHorizontalLayout ? Qt::Horizontal : Qt::Vertical);
	m_btnToggleOrientation->setText(m_isHorizontalLayout ? QStringLiteral("⬌ Lado a Lado") : QStringLiteral("⬍ Apilado"));
	saveSettings();
}

void ReplayDock::setupStyles()
{
	setStyleSheet(QStringLiteral(
		"QPushButton { padding: 2px 5px; font-size: 9pt; }"
		"QComboBox { padding: 2px 3px; font-size: 9pt; }"
		"QLineEdit { padding: 2px 3px; font-size: 9pt; }"
		"QLabel { font-size: 9pt; }"
		"QGroupBox { font-size: 9pt; font-weight: bold; margin-top: 3px; padding-top: 5px; }"
		"QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 3px; }"
		"QTableWidget { gridline-color: #424242; font-size: 9pt; }"
		"QTableWidget::item:selected { background-color: #1976d2; color: white; }"
		"QSplitter::handle { background-color: #424242; }"));
}

// ===========================================================================
// Configuración y Escenas
// ===========================================================================
void ReplayDock::loadSettings()
{
	config_t *cfg = obs_frontend_get_app_config();
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

	const char *horiz = config_get_string(cfg, kConfigSection, "HorizontalLayout");
	if (horiz) {
		m_isHorizontalLayout = (std::strcmp(horiz, "true") == 0);
		if (m_mainSplitter)
			m_mainSplitter->setOrientation(m_isHorizontalLayout ? Qt::Horizontal : Qt::Vertical);
		if (m_btnToggleOrientation)
			m_btnToggleOrientation->setText(m_isHorizontalLayout ? QStringLiteral("⬌ Lado a Lado") : QStringLiteral("⬍ Apilado"));
	}

	const char *splitState = config_get_string(cfg, kConfigSection, "SplitterState");
	if (splitState && m_mainSplitter && *splitState) {
		m_mainSplitter->restoreState(QByteArray::fromBase64(splitState));
	}
}

void ReplayDock::saveSettings()
{
	config_t *cfg = obs_frontend_get_app_config();
	if (!cfg)
		return;

	config_set_string(cfg, kConfigSection, "ReplayFolder", m_txtFolder->text().toUtf8().constData());
	config_set_string(cfg, kConfigSection, "LiveScene", m_engine.liveScene().toUtf8().constData());
	config_set_string(cfg, kConfigSection, "HorizontalLayout", m_isHorizontalLayout ? "true" : "false");
	if (m_mainSplitter) {
		const QByteArray splitData = m_mainSplitter->saveState().toBase64();
		config_set_string(cfg, kConfigSection, "SplitterState", splitData.constData());
	}
	config_save(cfg);
}

void ReplayDock::refreshSceneList()
{
	char **scenes = obs_frontend_get_scene_names();
	if (!scenes)
		return;

	m_comboLiveScene->blockSignals(true);
	m_comboLiveScene->clear();

	const QString targetLive = m_engine.liveScene();
	int selectIndex = -1;

	for (size_t i = 0; scenes[i] != nullptr; ++i) {
		const QString sName = QString::fromUtf8(scenes[i]);
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

	// Actualizar dinámicamente el selector de cámaras con las encontradas en disco
	const QString currentCam = m_comboCameraFilter->currentText();
	m_comboCameraFilter->blockSignals(true);
	m_comboCameraFilter->clear();
	m_comboCameraFilter->addItem(QStringLiteral("Todas las cámaras"), QString());

	QStringList foundCameras;
	bool hasPrincipal = false;
	for (const ClipInfo &c : m_allClips) {
		if (c.isPrincipal) {
			hasPrincipal = true;
		} else if (!c.cameraLabel().isEmpty() && !foundCameras.contains(c.cameraLabel())) {
			foundCameras.push_back(c.cameraLabel());
		}
	}
	if (hasPrincipal) {
		m_comboCameraFilter->addItem(QStringLiteral("Principal"), QStringLiteral("Principal"));
	}
	foundCameras.sort();
	for (const QString &cam : foundCameras) {
		m_comboCameraFilter->addItem(cam, cam);
	}

	const int idx = m_comboCameraFilter->findText(currentCam);
	if (idx >= 0)
		m_comboCameraFilter->setCurrentIndex(idx);
	else
		m_comboCameraFilter->setCurrentIndex(0);
	m_comboCameraFilter->blockSignals(false);

	populateClipTable();
	updateAngleButtons(m_activeDeckTab);
}

void ReplayDock::onFilterChanged()
{
	populateClipTable();
}

void ReplayDock::populateClipTable()
{
	const QString camFilter = m_comboCameraFilter->currentData().toString();
	const int teamFilter = m_filterTeamGroup->checkedId();
	const bool filter3PT = m_btnFilter3PT->isChecked();
	const bool filterTop = m_btnFilterTop->isChecked();
	const bool filterVar = m_btnFilterVar->isChecked();

	m_visibleClips.clear();
	for (const ClipInfo &c : m_allClips) {
		if (m_filterCustomPlaylistOnly) {
			bool inPl = false;
			for (const ClipInfo &p : m_customPlaylist) {
				if (p.filePath == c.filePath) {
					inPl = true;
					break;
				}
			}
			if (!inPl)
				continue;
		}

		if (!camFilter.isEmpty() && c.cameraLabel() != camFilter)
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

		const qint64 t = (c.eventTimeMs > 0) ? c.eventTimeMs : c.mtimeMs;
		const QString timeStr = QDateTime::fromMSecsSinceEpoch(t).toString(QStringLiteral("HH:mm:ss"));
		QTableWidgetItem *itemName = new QTableWidgetItem(QStringLiteral("%1  (%2)").arg(c.fileName, timeStr));

		QTableWidgetItem *itemCam = new QTableWidgetItem(c.cameraLabel());
		itemCam->setTextAlignment(Qt::AlignCenter);

		QString badges = tags.badges();
		for (const ClipInfo &p : m_customPlaylist) {
			if (p.filePath == c.filePath) {
				badges += QStringLiteral(" [★PL]");
				break;
			}
		}

		QTableWidgetItem *itemBadges = new QTableWidgetItem(badges);
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
}

// ===========================================================================
// Carga directa con 1 solo toque (alternancia inteligente A/B y Preview inmediato)
// ===========================================================================
void ReplayDock::onClipClicked(int row, int /*col*/)
{
	if (row < 0 || row >= m_visibleClips.size())
		return;

	const ClipInfo &clip = m_visibleClips[row];

	// Alternancia inteligente A/B:
	// Si Deck A está al aire en Programa, mandar al Deck B.
	// Si Deck B está al aire en Programa, mandar al Deck A.
	// Si ninguno está al aire, alternar en cada toque.
	const int onAirDeck = m_engine.deckOnAir();
	int targetDeck = 0;
	if (onAirDeck == 0) {
		targetDeck = 1;
	} else if (onAirDeck == 1) {
		targetDeck = 0;
	} else {
		targetDeck = (m_lastTappedDeck == 0) ? 1 : 0;
	}

	m_lastTappedDeck = targetDeck;
	m_activeDeckTab = targetDeck;

	// Cargar clip pausado en el primer cuadro (cued)
	m_engine.loadClip(targetDeck, clip, false);

	// Actualizar selección visual de Deck
	m_btnSelectDeckA->setChecked(m_activeDeckTab == 0);
	m_btnSelectDeckB->setChecked(m_activeDeckTab == 1);
	updateTransportUi(m_activeDeckTab);
	updateAngleButtons(m_activeDeckTab);

	// Abrir de inmediato la escena correspondiente en el Preview de OBS Studio
	m_engine.sendToPreview(targetDeck);
}

void ReplayDock::onClipDoubleClicked(int row, int col)
{
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
		m_engine.sendToPreview(targetDeck);
		return;
	}

	onClipClicked(row, col);
}

void ReplayDock::onClipContextMenu(const QPoint &pos)
{
	const int row = m_tableClips->rowAt(pos.y());
	if (row < 0 || row >= m_visibleClips.size())
		return;

	const ClipInfo clip = m_visibleClips[row];
	QMenu menu(this);

	QAction *actA = menu.addAction(QStringLiteral("▶ Cargar en Deck A (REPLAY)"));
	QAction *actB = menu.addAction(QStringLiteral("▶ Cargar en Deck B (REPLAY 2)"));
	menu.addSeparator();

	bool inPlaylist = false;
	for (const ClipInfo &c : m_customPlaylist) {
		if (c.filePath == clip.filePath) {
			inPlaylist = true;
			break;
		}
	}

	QAction *actPl = nullptr;
	if (!inPlaylist) {
		actPl = menu.addAction(QStringLiteral("➕ Añadir a Mi Playlist"));
	} else {
		actPl = menu.addAction(QStringLiteral("➖ Quitar de Mi Playlist"));
	}

	QAction *chosen = menu.exec(m_tableClips->viewport()->mapToGlobal(pos));
	if (chosen == actA) {
		m_engine.loadClip(0, clip, false);
		m_activeDeckTab = 0;
		m_btnSelectDeckA->setChecked(true);
		m_btnSelectDeckB->setChecked(false);
		updateTransportUi(0);
		updateAngleButtons(0);
		m_engine.sendToPreview(0);
	} else if (chosen == actB) {
		m_engine.loadClip(1, clip, false);
		m_activeDeckTab = 1;
		m_btnSelectDeckA->setChecked(false);
		m_btnSelectDeckB->setChecked(true);
		updateTransportUi(1);
		updateAngleButtons(1);
		m_engine.sendToPreview(1);
	} else if (chosen == actPl) {
		if (!inPlaylist) {
			m_customPlaylist.push_back(clip);
			m_lblStatus->setText(QStringLiteral("Añadido a Playlist: %1").arg(clip.displayName()));
		} else {
			for (int i = 0; i < m_customPlaylist.size(); ++i) {
				if (m_customPlaylist[i].filePath == clip.filePath) {
					m_customPlaylist.removeAt(i);
					break;
				}
			}
			m_lblStatus->setText(QStringLiteral("Eliminado de Playlist: %1").arg(clip.displayName()));
		}
		if (m_btnFilterPlaylist)
			m_btnFilterPlaylist->setText(QStringLiteral("★ Mi Playlist (%1)").arg(m_customPlaylist.size()));
		populateClipTable();
	}
}

// ===========================================================================
// Gestión de Playlist Personalizada
// ===========================================================================
void ReplayDock::onAddToCustomPlaylist()
{
	const QModelIndexList selRows = m_tableClips->selectionModel()->selectedRows();
	if (selRows.isEmpty()) {
		m_lblStatus->setText(QStringLiteral("Selecciona al menos un clip de la tabla para añadir a la playlist."));
		return;
	}

	int addedCount = 0;
	for (const QModelIndex &idx : selRows) {
		const int r = idx.row();
		if (r < 0 || r >= m_visibleClips.size())
			continue;
		const ClipInfo &c = m_visibleClips[r];
		bool exists = false;
		for (const ClipInfo &p : m_customPlaylist) {
			if (p.filePath == c.filePath) {
				exists = true;
				break;
			}
		}
		if (!exists) {
			m_customPlaylist.push_back(c);
			addedCount++;
		}
	}

	if (m_btnFilterPlaylist)
		m_btnFilterPlaylist->setText(QStringLiteral("★ Mi Playlist (%1)").arg(m_customPlaylist.size()));

	m_lblStatus->setText(QStringLiteral("%1 jugadas añadidas a Mi Playlist (Total: %2).")
				 .arg(addedCount)
				 .arg(m_customPlaylist.size()));
	populateClipTable();
}

void ReplayDock::onToggleCustomPlaylistFilter()
{
	m_filterCustomPlaylistOnly = !m_filterCustomPlaylistOnly;
	if (m_btnFilterPlaylist) {
		m_btnFilterPlaylist->setStyleSheet(m_filterCustomPlaylistOnly
			? QStringLiteral("background-color: #f57f17; color: white; font-weight: bold;")
			: QStringLiteral(""));
	}
	populateClipTable();
}

void ReplayDock::onLoadCustomPlaylist()
{
	if (m_customPlaylist.isEmpty()) {
		m_lblStatus->setText(QStringLiteral("Mi Playlist está vacía. Añade jugadas con [+ Playlist]."));
		return;
	}

	const int targetDeck = m_engine.pickFreeDeck(m_activeDeckTab);
	m_engine.loadPlaylist(targetDeck, m_customPlaylist);
	m_activeDeckTab = targetDeck;
	m_btnSelectDeckA->setChecked(m_activeDeckTab == 0);
	m_btnSelectDeckB->setChecked(m_activeDeckTab == 1);
	updateTransportUi(m_activeDeckTab);
	m_engine.sendToPreview(targetDeck);
	m_lblStatus->setText(QStringLiteral("Mi Playlist (%1 jugadas) cargada en Deck %2.")
				 .arg(m_customPlaylist.size())
				 .arg(QString::fromUtf8(kDeckLabels[targetDeck])));
}

void ReplayDock::onClearCustomPlaylist()
{
	m_customPlaylist.clear();
	if (m_btnFilterPlaylist) {
		m_btnFilterPlaylist->setText(QStringLiteral("★ Mi Playlist (0)"));
		m_btnFilterPlaylist->setStyleSheet(QStringLiteral(""));
	}
	m_filterCustomPlaylistOnly = false;
	m_lblStatus->setText(QStringLiteral("Mi Playlist ha sido vaciada."));
	populateClipTable();
}

// ===========================================================================
// Etiquetas Deportivas
// ===========================================================================
void ReplayDock::onTagClicked()
{
	const QPushButton *senderBtn = qobject_cast<QPushButton *>(sender());
	if (!senderBtn)
		return;

	const QModelIndexList selRows = m_tableClips->selectionModel()->selectedRows();
	if (selRows.isEmpty()) {
		m_lblStatus->setText(QStringLiteral("Selecciona uno o más clips en la tabla para etiquetarlos."));
		return;
	}

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

	std::sort(summaryClips.begin(), summaryClips.end(), [](const ClipInfo &a, const ClipInfo &b) {
		const qint64 ta = (a.eventTimeMs > 0) ? a.eventTimeMs : a.mtimeMs;
		const qint64 tb = (b.eventTimeMs > 0) ? b.eventTimeMs : b.mtimeMs;
		return ta < tb;
	});

	const int targetDeck = m_engine.pickFreeDeck(m_activeDeckTab);
	m_engine.loadPlaylist(targetDeck, summaryClips);
	m_activeDeckTab = targetDeck;
	m_btnSelectDeckA->setChecked(m_activeDeckTab == 0);
	m_btnSelectDeckB->setChecked(m_activeDeckTab == 1);
	updateTransportUi(m_activeDeckTab);
	m_engine.sendToPreview(targetDeck);
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
// Multi-cámara instantánea (Ángulos con nombres reales)
// ===========================================================================
void ReplayDock::updateAngleButtons(int deck)
{
	qDeleteAll(m_angleButtons);
	m_angleButtons.clear();

	const ClipInfo current = m_engine.currentClip(deck);
	if (!current.isValid() || current.groupKey.isEmpty())
		return;

	// Buscar todos los clips del mismo grupo multi-ángulo sincronizado
	for (const ClipInfo &c : m_allClips) {
		if (c.groupKey == current.groupKey) {
			QPushButton *btn = new QPushButton(c.cameraLabel(), this);
			btn->setCheckable(true);
			btn->setChecked(c.filePath == current.filePath);
			if (btn->isChecked()) {
				btn->setStyleSheet(QStringLiteral("font-weight: bold; background-color: #2e7d32; color: white; padding: 2px 6px;"));
			} else {
				btn->setStyleSheet(QStringLiteral("padding: 2px 6px;"));
			}

			connect(btn, &QPushButton::clicked, this, [this, c]() {
				m_engine.switchAngle(m_activeDeckTab, c);
				updateAngleButtons(m_activeDeckTab);
				m_engine.sendToPreview(m_activeDeckTab);
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
