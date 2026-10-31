#pragma once

// ---------------------------------------------------------------------------
// Motor de reproducción A/B (doble casilla)
//
// Reglas de oro para no congelar OBS:
//  1. TODAS las llamadas a libobs se hacen desde el hilo de UI (Qt main thread).
//  2. Los callbacks de señales de libobs (que llegan desde el hilo de media)
//     solo incrementan contadores atómicos; el procesamiento ocurre en un
//     QTimer del hilo de UI (onTick).
//  3. Los cambios de velocidad se agrupan (debounce) y nunca se aplican
//     mientras la fuente está abriendo un archivo: el demuxer de ffmpeg
//     no recibe ráfagas de reinicios.
//  4. Los cambios entre clips de una playlist se hacen dentro de la MISMA
//     fuente => no se dispara la transición de escena (Stinger).
// ---------------------------------------------------------------------------

#include "replay-types.hpp"

#include <obs.h>

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <array>
#include <atomic>
#include <memory>

namespace bbr {

struct ObsSourceDeleter {
	void operator()(obs_source_t *s) const noexcept { obs_source_release(s); }
};
using SourcePtr = std::unique_ptr<obs_source_t, ObsSourceDeleter>;

struct ObsDataDeleter {
	void operator()(obs_data_t *d) const noexcept { obs_data_release(d); }
};
using DataPtr = std::unique_ptr<obs_data_t, ObsDataDeleter>;

class ReplayEngine : public QObject {
	Q_OBJECT

public:
	explicit ReplayEngine(QObject *parent = nullptr);
	~ReplayEngine() override;

	// Ciclo de vida -----------------------------------------------------------
	void handleFrontendEvent(int event); // enum obs_frontend_event
	void setReady(bool ready);
	bool isReady() const { return m_ready; }
	void shutdown();

	// Configuración -----------------------------------------------------------
	void setLiveScene(const QString &name) { m_liveScene = name; }
	QString liveScene() const { return m_liveScene; }
	void setAutoReturn(bool on) { m_autoReturn = on; }
	void setHwDecode(bool on) { m_hwDecode = on; }
	void setSyncMode(SyncMode mode) { m_syncMode = mode; }

	// Casillas ----------------------------------------------------------------
	bool ensureDecks();
	QString deckSceneName(int deck) const;
	int deckOnAir() const;
	bool isOnAir(int deck) const;
	bool isInPreview(int deck) const;
	int pickFreeDeck(int preferred) const;

	// Carga -------------------------------------------------------------------
	bool loadClip(int deck, const ClipInfo &clip, bool autoplay = false);
	bool loadPlaylist(int deck, const ClipList &clips);
	bool switchAngle(int deck, const ClipInfo &clip);
	void clearPlaylist(int deck);

	// Transporte --------------------------------------------------------------
	void play(int deck);
	void pause(int deck);
	void togglePlayPause(int deck);
	void stop(int deck);
	void stepFrame(int deck, int direction);
	void seek(int deck, qint64 ms);
	void setSpeed(int deck, int percent, bool immediate = false);

	// Escenas -----------------------------------------------------------------
	void sendToPreview(int deck);
	void takeToProgram(int deck);
	void returnToLive();

	// Consultas ---------------------------------------------------------------
	ClipInfo currentClip(int deck) const;
	int playlistIndex(int deck) const;
	int playlistSize(int deck) const;
	ClipList playlist(int deck) const;
	int speed(int deck) const;
	qint64 mediaTime(int deck) const;
	qint64 mediaDuration(int deck) const;
	obs_media_state mediaState(int deck) const;
	bool isLoading(int deck) const;
	double frameDurationMs() const;

signals:
	void deckChanged(int deck);
	void sceneStateChanged();
	void statusMessage(const QString &message);

private:
	struct PendingStart {
		bool active = false;
		uint32_t startedSnap = 0;
		qint64 seekMs = 0;      // absoluto, o "desde el final" si alignEnd
		bool alignEnd = false;
		bool pauseAfter = false;
		QElapsedTimer timer;
	};

	struct Deck {
		int index = 0;
		QString sceneName;
		QString sourceName;
		obs_weak_source_t *weak = nullptr;

		// Escritos desde el hilo de media de OBS, leídos en onTick()
		std::atomic<uint32_t> startedCount{0};
		std::atomic<uint32_t> endedCount{0};
		uint32_t endedSnap = 0;

		ClipInfo clip;
		ClipList playlist;
		int playlistIndex = -1;

		int speed = 100;        // velocidad aplicada en la fuente (1..100)
		int desiredSpeed = 100; // velocidad pedida por la UI (0..100, 0 = pausa)
		bool pausedByZero = false;
		bool resumeAfterSpeed = false;

		PendingStart pending;

		double stepPos = -1.0;
		QElapsedTimer stepTimer;

		QTimer *speedTimer = nullptr;
	};

	bool validDeck(int i) const { return i >= 0 && i < kDeckCount; }

	SourcePtr resolveSource(Deck &d, bool create);
	SourcePtr peekSource(const Deck &d) const;
	void detachDeck(Deck &d);

	void applyMediaSettings(obs_source_t *src, const QString &path, int speed);
	void beginPending(Deck &d, qint64 seekMs, bool alignEnd, bool pauseAfter);
	bool loadInternal(Deck &d, const ClipInfo &clip, bool autoplay, qint64 seekMs, bool alignEnd);
	void processPending(Deck &d, obs_source_t *src);
	void handleEnded(Deck &d);
	void applySpeed(int deck);
	void onTick();

	std::array<Deck, kDeckCount> m_decks;
	QTimer m_tick;

	QString m_liveScene = QString::fromUtf8(kDefaultLiveScene);
	bool m_autoReturn = true;
	bool m_hwDecode = false;
	SyncMode m_syncMode = SyncMode::SameMillisecond;
	bool m_ready = false;
	bool m_shutdown = false;
};

} // namespace bbr
