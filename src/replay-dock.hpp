#pragma once

// ---------------------------------------------------------------------------
// Interfaz de usuario QDockWidget para obs-basketball-replay
// Integrada nativamente con Qt 6 y la paleta/estilo oscuro de OBS Studio
// ---------------------------------------------------------------------------

#include "clip-scanner.hpp"
#include "replay-engine.hpp"
#include "replay-types.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace bbr {

class ReplayDock : public QDockWidget {
	Q_OBJECT

public:
	explicit ReplayDock(QWidget *parent = nullptr);
	~ReplayDock() override;

	void init();

public slots:
	void refreshSceneList();

private slots:
	// Escaneo y clips
	void onBrowseFolder();
	void onClipsUpdated(const bbr::ClipList &clips);
	void onFilterChanged();
	void onClipTableSelectionChanged();
	void onClipDoubleClicked(int row, int col);

	// Etiquetas deportivas
	void onTagClicked();
	void onHalftimeSummaryClicked();

	// Transporte y control de Deck
	void onDeckTabChanged(int index);
	void onPlayPauseClicked();
	void onStopClicked();
	void onStepFrameBack();
	void onStepFrameFwd();
	void onSpeedSliderChanged(int value);
	void onSpeedQuickBtn(int percent);
	void onTimelineSliderMoved(int position);
	void onTimelineSliderReleased();

	// Transición / Escenas
	void onSendPreviewClicked();
	void onTakeProgramClicked();
	void onReturnLiveClicked();
	void onLiveSceneComboChanged(int index);

	// Multi-cámara instantánea
	void onCameraSwitchClicked(int camIndex);

	// Actualización periódica de UI
	void onEngineDeckChanged(int deck);
	void onEngineSceneStateChanged();
	void onEngineStatusMessage(const QString &msg);
	void onUiTick();

private:
	void setupUi();
	void setupStyles();
	void loadSettings();
	void saveSettings();
	void populateClipTable();
	void updateTransportUi(int deck);
	void updateAngleButtons(int deck);
	void updateDeckStatusBadges();

	// Componentes principales de lógica
	ClipScanner m_scanner;
	TagStore m_tagStore;
	ReplayEngine m_engine;

	// Datos en memoria
	ClipList m_allClips;
	ClipList m_visibleClips;
	int m_activeDeckTab = 0; // 0 = Deck A, 1 = Deck B
	bool m_isScrubbing = false;

	// Widgets UI - Barra Superior
	QLineEdit *m_txtFolder = nullptr;
	QPushButton *m_btnBrowse = nullptr;
	QComboBox *m_comboLiveScene = nullptr;
	QPushButton *m_btnReturnLive = nullptr;

	// Filtros Rápidos
	QComboBox *m_comboCameraFilter = nullptr;
	QButtonGroup *m_filterTeamGroup = nullptr;
	QPushButton *m_btnFilterAll = nullptr;
	QPushButton *m_btnFilterHome = nullptr;
	QPushButton *m_btnFilterAway = nullptr;
	QPushButton *m_btnFilter3PT = nullptr;
	QPushButton *m_btnFilterTop = nullptr;
	QPushButton *m_btnFilterVar = nullptr;
	QPushButton *m_btnHalftimeSummary = nullptr;

	// Tabla de Clips
	QTableWidget *m_tableClips = nullptr;

	// Panel de Etiquetado Rápido para selección
	QPushButton *m_btnTagHome = nullptr;
	QPushButton *m_btnTagAway = nullptr;
	QPushButton *m_btnTag3PT = nullptr;
	QPushButton *m_btnTagTop = nullptr;
	QPushButton *m_btnTagVar = nullptr;
	QPushButton *m_btnTagClear = nullptr;

	// Pestañas / Selector de Deck A / B
	QPushButton *m_btnSelectDeckA = nullptr;
	QPushButton *m_btnSelectDeckB = nullptr;
	QLabel *m_lblDeckAStatus = nullptr;
	QLabel *m_lblDeckBStatus = nullptr;

	// Barra de Ángulos Multi-Cámara
	QHBoxLayout *m_angleLayout = nullptr;
	QVector<QPushButton *> m_angleButtons;

	// Controles de Transporte
	QLabel *m_lblCurrentClipTitle = nullptr;
	QLabel *m_lblTime = nullptr;
	QSlider *m_sliderTimeline = nullptr;
	QPushButton *m_btnStepBack = nullptr;
	QPushButton *m_btnPlayPause = nullptr;
	QPushButton *m_btnStepFwd = nullptr;
	QPushButton *m_btnStop = nullptr;

	// Controles de Velocidad
	QSlider *m_sliderSpeed = nullptr;
	QLabel *m_lblSpeedVal = nullptr;
	QPushButton *m_btnSpeed25 = nullptr;
	QPushButton *m_btnSpeed50 = nullptr;
	QPushButton *m_btnSpeed75 = nullptr;
	QPushButton *m_btnSpeed100 = nullptr;

	// Botones de Emisión
	QPushButton *m_btnSendPreview = nullptr;
	QPushButton *m_btnTakeProgram = nullptr;

	// Barra de Estado
	QLabel *m_lblStatus = nullptr;

	// Timer de actualización de Timeline
	QTimer m_uiTimer;
};

} // namespace bbr
