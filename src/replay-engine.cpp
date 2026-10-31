#include "replay-engine.hpp"

#include <obs-frontend-api.h>
#include <graphics/vec2.h>
#include <util/base.h>

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bbr {

namespace {

constexpr int kTickMs = 40;
constexpr int kSpeedDebounceMs = 200;
constexpr qint64 kPendingFallbackMs = 2500;
constexpr qint64 kPendingTimeoutMs = 10000;
constexpr qint64 kStepMemoryMs = 4000;
constexpr int kPauseAfterReturnMs = 2500;

// Callback de señal de libobs (hilo de media): solo incrementa un contador.
void onMediaCounter(void *data, calldata_t *)
{
	static_cast<std::atomic<uint32_t> *>(data)->fetch_add(1, std::memory_order_relaxed);
}

SourcePtr sourceByName(const QString &name)
{
	if (name.isEmpty())
		return {};
	return SourcePtr(obs_get_source_by_name(name.toUtf8().constData()));
}

QString programSceneName()
{
	SourcePtr s(obs_frontend_get_current_scene());
	return s ? QString::fromUtf8(obs_source_get_name(s.get())) : QString();
}

QString previewSceneName()
{
	if (!obs_frontend_preview_program_mode_active())
		return {};
	SourcePtr s(obs_frontend_get_current_preview_scene());
	return s ? QString::fromUtf8(obs_source_get_name(s.get())) : QString();
}

bool isMediaSource(obs_source_t *src)
{
	const char *id = src ? obs_source_get_unversioned_id(src) : nullptr;
	return id && std::strcmp(id, kMediaSourceId) == 0;
}

struct FindMediaCtx {
	const char *preferredName = nullptr;
	obs_source_t *preferred = nullptr;
	obs_source_t *first = nullptr;
};

bool findMediaInScene(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<FindMediaCtx *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!isMediaSource(src))
		return true;

	if (!ctx->first)
		ctx->first = src;
	const char *name = obs_source_get_name(src);
	if (name && ctx->preferredName && std::strcmp(name, ctx->preferredName) == 0) {
		ctx->preferred = src;
		return false;
	}
	return true;
}

void fitItemToCanvas(obs_sceneitem_t *item)
{
	obs_video_info ovi{};
	if (!item || !obs_get_video_info(&ovi))
		return;
	vec2 bounds;
	vec2_set(&bounds, float(ovi.base_width), float(ovi.base_height));
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, 0); // centrado
	obs_sceneitem_set_bounds(item, &bounds);
}

} // namespace

// ===========================================================================
// Construcción / ciclo de vida
// ===========================================================================
ReplayEngine::ReplayEngine(QObject *parent) : QObject(parent)
{
	for (int i = 0; i < kDeckCount; ++i) {
		Deck &d = m_decks[i];
		d.index = i;
		d.sceneName = QString::fromUtf8(kDeckSceneNames[i]);
		d.sourceName = QString::fromUtf8(kDeckSourceNames[i]);
		d.speedTimer = new QTimer(this);
		d.speedTimer->setSingleShot(true);
		d.speedTimer->setInterval(kSpeedDebounceMs);
		connect(d.speedTimer, &QTimer::timeout, this, [this, i]() { applySpeed(i); });
	}

	m_tick.setInterval(kTickMs);
	m_tick.setTimerType(Qt::PreciseTimer);
	connect(&m_tick, &QTimer::timeout, this, &ReplayEngine::onTick);
	m_tick.start();
}

ReplayEngine::~ReplayEngine()
{
	shutdown();
}

void ReplayEngine::shutdown()
{
	if (m_shutdown)
		return;
	m_shutdown = true;
	m_tick.stop();
	for (Deck &d : m_decks) {
		if (d.speedTimer)
			d.speedTimer->stop();
		detachDeck(d);
	}
	m_ready = false;
}

void ReplayEngine::setReady(bool ready)
{
	if (m_shutdown)
		return;
	m_ready = ready;
	if (!ready) {
		for (Deck &d : m_decks)
			detachDeck(d);
	}
}

void ReplayEngine::handleFrontendEvent(int event)
{
	switch (static_cast<obs_frontend_event>(event)) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		setReady(true);
		ensureDecks();
		emit sceneStateChanged();
		break;

	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		setReady(false);
		break;

	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		emit sceneStateChanged();
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		shutdown();
		break;

	default:
		break;
	}
}

// ===========================================================================
// Resolución de escenas / fuentes
// ===========================================================================
SourcePtr ReplayEngine::peekSource(const Deck &d) const
{
	if (!d.weak)
		return {};
	SourcePtr s(obs_weak_source_get_source(d.weak));
	if (s && obs_source_removed(s.get()))
		s.reset();
	return s;
}

void ReplayEngine::detachDeck(Deck &d)
{
	d.pending.active = false;
	if (!d.weak)
		return;

	SourcePtr s(obs_weak_source_get_source(d.weak));
	if (s) {
		signal_handler_t *sh = obs_source_get_signal_handler(s.get());
		signal_handler_disconnect(sh, "media_started", onMediaCounter, &d.startedCount);
		signal_handler_disconnect(sh, "media_ended", onMediaCounter, &d.endedCount);
	}
	obs_weak_source_release(d.weak);
	d.weak = nullptr;
}

SourcePtr ReplayEngine::resolveSource(Deck &d, bool create)
{
	if (d.weak) {
		SourcePtr s = peekSource(d);
		if (s)
			return s;
		detachDeck(d); // la fuente fue eliminada por el usuario
	}

	if (!m_ready || m_shutdown)
		return {};

	const QByteArray sceneNameUtf8 = d.sceneName.toUtf8();
	const QByteArray sourceNameUtf8 = d.sourceName.toUtf8();

	// --- Escena de la casilla ---------------------------------------------
	SourcePtr sceneSrc = sourceByName(d.sceneName);
	if (sceneSrc && !obs_source_is_scene(sceneSrc.get())) {
		emit statusMessage(QStringLiteral("El nombre \"%1\" está en uso por una fuente que no es escena.").arg(d.sceneName));
		return {};
	}
	if (!sceneSrc) {
		if (!create)
			return {};
		obs_scene_t *created = obs_scene_create(sceneNameUtf8.constData());
		if (!created)
			return {};
		sceneSrc.reset(obs_source_get_ref(obs_scene_get_source(created)));
		obs_scene_release(created);
		if (!sceneSrc)
			return {};
		blog(LOG_INFO, "%s Escena creada: %s", kLogPrefix, sceneNameUtf8.constData());
	}

	obs_scene_t *scene = obs_scene_from_source(sceneSrc.get());
	if (!scene)
		return {};

	// --- Fuente multimedia dentro de la escena ------------------------------
	FindMediaCtx ctx;
	ctx.preferredName = sourceNameUtf8.constData();
	obs_scene_enum_items(scene, findMediaInScene, &ctx);

	SourcePtr media;
	if (obs_source_t *found = ctx.preferred ? ctx.preferred : ctx.first)
		media.reset(obs_source_get_ref(found));

	if (!media) {
		if (!create)
			return {};

		SourcePtr existing = sourceByName(d.sourceName);
		if (existing && isMediaSource(existing.get())) {
			media = std::move(existing);
		} else {
			DataPtr settings(obs_data_create());
			obs_data_set_bool(settings.get(), "is_local_file", true);
			obs_data_set_bool(settings.get(), "looping", false);
			obs_data_set_bool(settings.get(), "restart_on_activate", false);
			obs_data_set_bool(settings.get(), "close_when_inactive", false);
			obs_data_set_bool(settings.get(), "clear_on_media_end", false);
			obs_data_set_bool(settings.get(), "hw_decode", m_hwDecode);
			const QByteArray name = existing ? (d.sourceName + QStringLiteral(" (media)")).toUtf8() : sourceNameUtf8;
			media.reset(obs_source_create(kMediaSourceId, name.constData(), settings.get(), nullptr));
		}
		if (!media)
			return {};

		obs_sceneitem_t *item = obs_scene_add(scene, media.get());
		fitItemToCanvas(item);
		blog(LOG_INFO, "%s Fuente multimedia '%s' añadida a '%s'", kLogPrefix, obs_source_get_name(media.get()),
		     sceneNameUtf8.constData());
	}

	// --- Enganche de señales ------------------------------------------------
	d.weak = obs_source_get_weak_source(media.get());
	signal_handler_t *sh = obs_source_get_signal_handler(media.get());
	signal_handler_connect(sh, "media_started", onMediaCounter, &d.startedCount);
	signal_handler_connect(sh, "media_ended", onMediaCounter, &d.endedCount);
	d.endedSnap = d.endedCount.load();

	blog(LOG_INFO, "%s Deck %s -> fuente '%s' en escena '%s'", kLogPrefix, kDeckLabels[d.index],
	     obs_source_get_name(media.get()), sceneNameUtf8.constData());
	return media;
}

bool ReplayEngine::ensureDecks()
{
	bool ok = true;
	for (Deck &d : m_decks)
		ok = resolveSource(d, true) != nullptr && ok;
	return ok;
}

// ===========================================================================
// Estado de escenas
// ===========================================================================
QString ReplayEngine::deckSceneName(int deck) const
{
	return validDeck(deck) ? m_decks[deck].sceneName : QString();
}

int ReplayEngine::deckOnAir() const
{
	const QString program = programSceneName();
	for (const Deck &d : m_decks)
		if (!program.isEmpty() && program == d.sceneName)
			return d.index;
	return -1;
}

bool ReplayEngine::isOnAir(int deck) const
{
	return validDeck(deck) && deckOnAir() == deck;
}

bool ReplayEngine::isInPreview(int deck) const
{
	return validDeck(deck) && previewSceneName() == m_decks[deck].sceneName;
}

int ReplayEngine::pickFreeDeck(int preferred) const
{
	if (!validDeck(preferred))
		preferred = 0;
	const int air = deckOnAir();
	if (air < 0 || air != preferred)
		return preferred;
	return (air + 1) % kDeckCount;
}

// ===========================================================================
// Carga de clips
// ===========================================================================
void ReplayEngine::applyMediaSettings(obs_source_t *src, const QString &path, int speed)
{
	DataPtr s(obs_data_create());
	obs_data_set_bool(s.get(), "is_local_file", true);
	obs_data_set_string(s.get(), "local_file", path.toUtf8().constData());
	obs_data_set_bool(s.get(), "looping", false);
	obs_data_set_bool(s.get(), "restart_on_activate", false); // no reiniciar al entrar a Programa
	obs_data_set_bool(s.get(), "close_when_inactive", false);
	obs_data_set_bool(s.get(), "clear_on_media_end", false);  // mantener el último cuadro (sin negro)
	obs_data_set_bool(s.get(), "hw_decode", m_hwDecode);
	obs_data_set_int(s.get(), "speed_percent", std::clamp(speed, 1, 200));
	obs_source_update(src, s.get());
}

void ReplayEngine::beginPending(Deck &d, qint64 seekMs, bool alignEnd, bool pauseAfter)
{
	// Importante: se toma la "foto" del contador ANTES de disparar la apertura.
	d.pending.active = true;
	d.pending.startedSnap = d.startedCount.load();
	d.pending.seekMs = std::max<qint64>(0, seekMs);
	d.pending.alignEnd = alignEnd;
	d.pending.pauseAfter = pauseAfter;
	d.pending.timer.start();
}

bool ReplayEngine::loadInternal(Deck &d, const ClipInfo &clip, bool autoplay, qint64 seekMs, bool alignEnd)
{
	if (!clip.isValid())
		return false;
	if (!QFileInfo::exists(clip.filePath)) {
		emit statusMessage(QStringLiteral("Archivo no encontrado: %1").arg(clip.fileName));
		return false;
	}

	SourcePtr src = resolveSource(d, true);
	if (!src) {
		emit statusMessage(QStringLiteral("No se pudo preparar la escena \"%1\".").arg(d.sceneName));
		return false;
	}

	if (d.desiredSpeed == 0)
		autoplay = false;

	d.clip = clip;
	d.stepPos = -1.0;
	beginPending(d, seekMs, alignEnd, !autoplay);
	applyMediaSettings(src.get(), clip.filePath, d.speed);

	emit deckChanged(d.index);
	return true;
}

bool ReplayEngine::loadClip(int deck, const ClipInfo &clip, bool autoplay)
{
	if (!validDeck(deck))
		return false;
	Deck &d = m_decks[deck];
	d.playlist.clear();
	d.playlistIndex = -1;
	const bool ok = loadInternal(d, clip, autoplay, 0, false);
	if (ok)
		emit statusMessage(QStringLiteral("Deck %1: %2 cargado").arg(QString::fromUtf8(kDeckLabels[deck]), clip.displayName()));
	return ok;
}

bool ReplayEngine::loadPlaylist(int deck, const ClipList &clips)
{
	if (!validDeck(deck) || clips.isEmpty())
		return false;
	Deck &d = m_decks[deck];
	d.playlist = clips;
	d.playlistIndex = 0;
	const bool ok = loadInternal(d, clips.first(), false, 0, false);
	if (ok)
		emit statusMessage(QStringLiteral("Deck %1: playlist de %2 clips preparada").arg(QString::fromUtf8(kDeckLabels[deck])).arg(clips.size()));
	else
		clearPlaylist(deck);
	return ok;
}

void ReplayEngine::clearPlaylist(int deck)
{
	if (!validDeck(deck))
		return;
	m_decks[deck].playlist.clear();
	m_decks[deck].playlistIndex = -1;
	emit deckChanged(deck);
}

bool ReplayEngine::switchAngle(int deck, const ClipInfo &clip)
{
	if (!validDeck(deck) || !clip.isValid())
		return false;
	Deck &d = m_decks[deck];
	SourcePtr src = peekSource(d);

	qint64 t = 0;
	qint64 dur = 0;
	bool playing = false;

	if (src && d.clip.isValid()) {
		if (d.pending.active) {
			t = d.pending.alignEnd ? 0 : d.pending.seekMs;
			playing = !d.pending.pauseAfter;
		} else {
			const obs_media_state st = obs_source_media_get_state(src.get());
			t = obs_source_media_get_time(src.get());
			dur = obs_source_media_get_duration(src.get());
			playing = st == OBS_MEDIA_STATE_PLAYING;
			if (st == OBS_MEDIA_STATE_ENDED)
				t = dur;
		}
		if (d.stepPos >= 0.0 && !playing)
			t = std::llround(d.stepPos);
	}

	const bool alignEnd = m_syncMode == SyncMode::AlignEnd && dur > 0;
	const qint64 seekMs = alignEnd ? std::max<qint64>(0, dur - t) : t;

	// Si hay playlist, se sustituye el clip actual por el nuevo ángulo
	if (d.playlistIndex >= 0 && d.playlistIndex < d.playlist.size())
		d.playlist[d.playlistIndex] = clip;

	const bool ok = loadInternal(d, clip, playing, seekMs, alignEnd);
	if (ok)
		emit statusMessage(QStringLiteral("Deck %1: ángulo %2 @ %3")
					   .arg(QString::fromUtf8(kDeckLabels[deck]), clip.cameraLongLabel(), formatTimeMs(t)));
	return ok;
}

// ===========================================================================
// Bucle de supervisión (hilo de UI)
// ===========================================================================
void ReplayEngine::onTick()
{
	if (m_shutdown)
		return;

	const int air = deckOnAir();
	if (air >= 0) {
		if (m_lastAirDeck != air) {
			m_lastAirDeck = air;
			// Cuando una casilla entra al aire (Programa), jamás puede quedarse en pausa
			Deck &dAir = m_decks[air];
			if (dAir.desiredSpeed <= 0)
				dAir.desiredSpeed = (dAir.speed > 0) ? dAir.speed : 100;
			dAir.pausedByZero = false;
			if (dAir.pending.active)
				dAir.pending.pauseAfter = false;
			SourcePtr srcAir = peekSource(dAir);
			if (srcAir) {
				const obs_media_state st = obs_source_media_get_state(srcAir.get());
				if (st == OBS_MEDIA_STATE_PAUSED || st == OBS_MEDIA_STATE_STOPPED) {
					obs_source_media_play_pause(srcAir.get(), false);
				}
			}
		}
	} else {
		m_lastAirDeck = -1;
	}

	for (Deck &d : m_decks) {
		if (!d.weak)
			continue;
		SourcePtr src = peekSource(d);
		if (!src) {
			detachDeck(d);
			emit deckChanged(d.index);
			continue;
		}

		if (d.pending.active) {
			processPending(d, src.get());
			continue;
		}

		const uint32_t ended = d.endedCount.load();
		if (ended != d.endedSnap) {
			d.endedSnap = ended;
			handleEnded(d);
		}
	}
}

void ReplayEngine::processPending(Deck &d, obs_source_t *src)
{
	const bool started = d.startedCount.load() != d.pending.startedSnap;
	const obs_media_state st = obs_source_media_get_state(src);
	const qint64 dur = obs_source_media_get_duration(src);
	const bool running = st == OBS_MEDIA_STATE_PLAYING || st == OBS_MEDIA_STATE_PAUSED;
	const qint64 elapsed = d.pending.timer.elapsed();

	// Si el medio ya está abierto y corriendo con duración, está listo de inmediato (sin esperar 2.5s)
	const bool ready = (running && dur > 0) || started || (elapsed > 350);
	if (!ready) {
		if (elapsed > kPendingTimeoutMs || st == OBS_MEDIA_STATE_ERROR) {
			d.pending.active = false;
			d.endedSnap = d.endedCount.load();
			emit statusMessage(QStringLiteral("Deck %1: no se pudo abrir %2")
						   .arg(QString::fromUtf8(kDeckLabels[d.index]), d.clip.fileName));
			emit deckChanged(d.index);
		}
		return;
	}

	qint64 target = d.pending.alignEnd ? dur - d.pending.seekMs : d.pending.seekMs;
	target = std::clamp<qint64>(target, 0, std::max<qint64>(0, dur - 1));

	if (d.pending.pauseAfter) {
		obs_source_media_play_pause(src, true);
		obs_source_media_set_time(src, target); // cue exacto (incluso en 0)
	} else if (target > 0) {
		obs_source_media_set_time(src, target);
	}

	d.pending.active = false;
	d.endedSnap = d.endedCount.load();

	// Un cambio de velocidad pedido mientras se abría el archivo se aplica ahora
	if (d.desiredSpeed > 0 && (d.desiredSpeed != d.speed || d.resumeAfterSpeed))
		d.speedTimer->start(kSpeedDebounceMs);

	emit deckChanged(d.index);
}

void ReplayEngine::handleEnded(Deck &d)
{
	// Playlist: siguiente clip en la MISMA fuente => sin Stinger
	if (d.playlistIndex >= 0 && d.playlistIndex + 1 < d.playlist.size()) {
		d.playlistIndex++;
		const ClipInfo next = d.playlist[d.playlistIndex];
		if (loadInternal(d, next, true, 0, false)) {
			emit statusMessage(QStringLiteral("Deck %1: playlist %2/%3 - %4")
						   .arg(QString::fromUtf8(kDeckLabels[d.index]))
						   .arg(d.playlistIndex + 1)
						   .arg(d.playlist.size())
						   .arg(next.displayName()));
			return;
		}
	}

	if (m_autoReturn && isOnAir(d.index))
		returnToLive();

	emit deckChanged(d.index);
}

// ===========================================================================
// Transporte
// ===========================================================================
void ReplayEngine::play(int deck)
{
	if (!validDeck(deck))
		return;
	Deck &d = m_decks[deck];

	if (d.desiredSpeed == 0) {
		d.desiredSpeed = d.speed;
		d.pausedByZero = false;
	}
	d.stepPos = -1.0;

	if (d.pending.active) {
		d.pending.pauseAfter = false;
		emit deckChanged(deck);
		return;
	}

	SourcePtr src = resolveSource(d, false);
	if (!src || !d.clip.isValid())
		return;

	switch (obs_source_media_get_state(src.get())) {
	case OBS_MEDIA_STATE_PAUSED:
		obs_source_media_play_pause(src.get(), false);
		break;
	case OBS_MEDIA_STATE_ENDED:
		if (d.playlist.size() > 1 && d.playlistIndex == d.playlist.size() - 1) {
			d.playlistIndex = 0; // reiniciar la playlist completa
			loadInternal(d, d.playlist.first(), true, 0, false);
			return;
		}
		beginPending(d, 0, false, false);
		obs_source_media_restart(src.get());
		break;
	case OBS_MEDIA_STATE_PLAYING:
	case OBS_MEDIA_STATE_OPENING:
	case OBS_MEDIA_STATE_BUFFERING:
		break;
	default: // NONE / STOPPED / ERROR -> recargar
		loadInternal(d, d.clip, true, 0, false);
		return;
	}
	emit deckChanged(deck);
}

void ReplayEngine::pause(int deck)
{
	if (!validDeck(deck))
		return;
	Deck &d = m_decks[deck];
	if (d.pending.active) {
		d.pending.pauseAfter = true;
		emit deckChanged(deck);
		return;
	}
	SourcePtr src = peekSource(d);
	if (!src)
		return;
	if (obs_source_media_get_state(src.get()) == OBS_MEDIA_STATE_PLAYING)
		obs_source_media_play_pause(src.get(), true);
	emit deckChanged(deck);
}

void ReplayEngine::togglePlayPause(int deck)
{
	if (!validDeck(deck))
		return;
	const Deck &d = m_decks[deck];
	const bool playing = d.pending.active ? !d.pending.pauseAfter : mediaState(deck) == OBS_MEDIA_STATE_PLAYING;
	if (playing)
		pause(deck);
	else
		play(deck);
}

void ReplayEngine::stop(int deck)
{
	if (!validDeck(deck))
		return;
	Deck &d = m_decks[deck];
	d.stepPos = -1.0;

	if (d.pending.active) {
		d.pending.seekMs = 0;
		d.pending.alignEnd = false;
		d.pending.pauseAfter = true;
		emit deckChanged(deck);
		return;
	}

	SourcePtr src = peekSource(d);
	if (!src || !d.clip.isValid())
		return;

	const obs_media_state st = obs_source_media_get_state(src.get());
	if (st == OBS_MEDIA_STATE_ENDED || st == OBS_MEDIA_STATE_STOPPED) {
		beginPending(d, 0, false, true);
		obs_source_media_restart(src.get());
	} else {
		// "Stop" en un replay = volver al inicio en pausa (cue), sin descargar el archivo
		obs_source_media_play_pause(src.get(), true);
		obs_source_media_set_time(src.get(), 0);
	}
	emit deckChanged(deck);
}

void ReplayEngine::seek(int deck, qint64 ms)
{
	if (!validDeck(deck))
		return;
	Deck &d = m_decks[deck];
	d.stepPos = -1.0;
	ms = std::max<qint64>(0, ms);

	if (d.pending.active) {
		d.pending.seekMs = ms;
		d.pending.alignEnd = false;
		return;
	}

	SourcePtr src = peekSource(d);
	if (!src || !d.clip.isValid())
		return;

	const obs_media_state st = obs_source_media_get_state(src.get());
	if (st == OBS_MEDIA_STATE_ENDED || st == OBS_MEDIA_STATE_STOPPED) {
		beginPending(d, ms, false, true);
		obs_source_media_restart(src.get());
	} else {
		obs_source_media_set_time(src.get(), ms);
	}
}

void ReplayEngine::stepFrame(int deck, int direction)
{
	if (!validDeck(deck) || direction == 0)
		return;
	Deck &d = m_decks[deck];
	if (d.pending.active)
		return;

	SourcePtr src = peekSource(d);
	if (!src || !d.clip.isValid())
		return;

	const obs_media_state st = obs_source_media_get_state(src.get());
	const qint64 dur = obs_source_media_get_duration(src.get());
	if (dur <= 0)
		return;

	if (st == OBS_MEDIA_STATE_PLAYING)
		obs_source_media_play_pause(src.get(), true);

	double base;
	if (d.stepPos >= 0.0 && d.stepTimer.isValid() && d.stepTimer.elapsed() < kStepMemoryMs)
		base = d.stepPos; // evita deriva acumulada entre pasos consecutivos
	else if (st == OBS_MEDIA_STATE_ENDED)
		base = double(dur);
	else
		base = double(obs_source_media_get_time(src.get()));

	const double next = std::clamp(base + direction * frameDurationMs(), 0.0, double(std::max<qint64>(0, dur - 1)));
	d.stepPos = next;
	d.stepTimer.start();

	const qint64 target = std::llround(next);
	if (st == OBS_MEDIA_STATE_ENDED || st == OBS_MEDIA_STATE_STOPPED) {
		beginPending(d, target, false, true);
		obs_source_media_restart(src.get());
	} else {
		obs_source_media_set_time(src.get(), target);
	}
	emit deckChanged(deck);
}

void ReplayEngine::setSpeed(int deck, int percent, bool immediate)
{
	if (!validDeck(deck))
		return;
	Deck &d = m_decks[deck];
	percent = std::clamp(percent, 0, 100);
	d.desiredSpeed = percent;

	if (percent == 0) {
		// 0 % = congelar imagen (pausa), sin reiniciar el demuxer
		d.speedTimer->stop();
		const bool playing = d.pending.active ? !d.pending.pauseAfter : mediaState(deck) == OBS_MEDIA_STATE_PLAYING;
		if (playing) {
			pause(deck);
			d.pausedByZero = true;
		}
		emit deckChanged(deck);
		return;
	}

	if (d.pausedByZero) {
		d.pausedByZero = false;
		d.resumeAfterSpeed = true;
	}

	if (immediate) {
		d.speedTimer->stop();
		applySpeed(deck);
	} else {
		d.speedTimer->start(kSpeedDebounceMs);
	}
}

void ReplayEngine::applySpeed(int deck)
{
	if (!validDeck(deck) || m_shutdown)
		return;
	Deck &d = m_decks[deck];
	if (d.desiredSpeed <= 0)
		return;

	SourcePtr src = peekSource(d);
	if (!src || !d.clip.isValid()) {
		d.speed = d.desiredSpeed;
		d.resumeAfterSpeed = false;
		return;
	}

	if (d.desiredSpeed == d.speed) {
		if (d.resumeAfterSpeed) {
			d.resumeAfterSpeed = false;
			play(deck);
		}
		return;
	}

	const obs_media_state st = obs_source_media_get_state(src.get());
	qint64 t = obs_source_media_get_time(src.get());
	if (st == OBS_MEDIA_STATE_ENDED)
		t = obs_source_media_get_duration(src.get());
	if (d.stepPos >= 0.0 && st != OBS_MEDIA_STATE_PLAYING)
		t = std::llround(d.stepPos);

	const bool wasPlaying = (st == OBS_MEDIA_STATE_PLAYING) || d.resumeAfterSpeed;
	d.speed = d.desiredSpeed;
	d.resumeAfterSpeed = false;

	// Actualizar velocidad directamente en la configuración de la fuente para evitar recrear demuxer completo
	obs_data_t *settings = obs_source_get_settings(src.get());
	if (settings) {
		obs_data_set_int(settings, "speed_percent", std::clamp(d.speed, 1, 200));
		obs_source_update(src.get(), settings);
		obs_data_release(settings);
	}

	// Restaurar posición y estado de reproducción inmediatamente para evitar parpadeos o saltos a cuadro 0
	if (t > 0) {
		obs_source_media_set_time(src.get(), t);
	}

	if (!wasPlaying) {
		obs_source_media_play_pause(src.get(), true);
		if (t >= 0)
			obs_source_media_set_time(src.get(), t);
	} else {
		obs_source_media_play_pause(src.get(), false);
	}

	emit deckChanged(deck);
}

// ===========================================================================
// Escenas: Preview / Programa / Vivo
// ===========================================================================
void ReplayEngine::sendToPreview(int deck)
{
	if (!validDeck(deck))
		return;
	if (!obs_frontend_preview_program_mode_active()) {
		obs_frontend_set_preview_program_mode(true);
	}
	if (isOnAir(deck))
		return;
	SourcePtr scene = sourceByName(m_decks[deck].sceneName);
	if (scene && obs_source_is_scene(scene.get()))
		obs_frontend_set_current_preview_scene(scene.get());
}

void ReplayEngine::takeToProgram(int deck)
{
	if (!validDeck(deck))
		return;
	Deck &d = m_decks[deck];
	if (!d.clip.isValid()) {
		emit statusMessage(QStringLiteral("Deck %1 está vacío.").arg(QString::fromUtf8(kDeckLabels[deck])));
		return;
	}

	SourcePtr src = resolveSource(d, true);
	SourcePtr scene = sourceByName(d.sceneName);
	if (!src || !scene || !obs_source_is_scene(scene.get())) {
		emit statusMessage(QStringLiteral("No existe la escena \"%1\".").arg(d.sceneName));
		return;
	}

	// Obligatorio: la repetición siempre pasa a Programa en reproducción fluida (nunca frisada)
	if (d.desiredSpeed <= 0)
		d.desiredSpeed = (d.speed > 0) ? d.speed : 100;
	d.pausedByZero = false;
	d.pending.active = false; // Cancelar cualquier retraso pendiente

	const obs_media_state st = obs_source_media_get_state(src.get());
	if (st == OBS_MEDIA_STATE_ENDED || st == OBS_MEDIA_STATE_STOPPED) {
		obs_source_media_restart(src.get());
	}
	obs_source_media_play_pause(src.get(), false);

	if (!isOnAir(deck))
		obs_frontend_set_current_scene(scene.get()); // Dispara la transición (Stinger)

	obs_source_media_play_pause(src.get(), false);
	emit deckChanged(deck);
}

void ReplayEngine::returnToLive()
{
	SourcePtr live = sourceByName(m_liveScene);
	if (!live || !obs_source_is_scene(live.get())) {
		emit statusMessage(QStringLiteral("No existe la escena en vivo \"%1\".").arg(m_liveScene));
		return;
	}

	const int air = deckOnAir();
	obs_frontend_set_current_scene(live.get()); // Stinger de salida

	if (air >= 0) {
		// Pausar la casilla cuando la transición ya terminó (evita congelar el Stinger)
		QTimer::singleShot(kPauseAfterReturnMs, this, [this, air]() {
			if (!m_shutdown && !isOnAir(air) && mediaState(air) == OBS_MEDIA_STATE_PLAYING)
				pause(air);
		});
	}
}

// ===========================================================================
// Consultas
// ===========================================================================
ClipInfo ReplayEngine::currentClip(int deck) const
{
	return validDeck(deck) ? m_decks[deck].clip : ClipInfo();
}

int ReplayEngine::playlistIndex(int deck) const
{
	return validDeck(deck) ? m_decks[deck].playlistIndex : -1;
}

int ReplayEngine::playlistSize(int deck) const
{
	return validDeck(deck) ? int(m_decks[deck].playlist.size()) : 0;
}

ClipList ReplayEngine::playlist(int deck) const
{
	return validDeck(deck) ? m_decks[deck].playlist : ClipList();
}

int ReplayEngine::speed(int deck) const
{
	return validDeck(deck) ? m_decks[deck].desiredSpeed : 100;
}

qint64 ReplayEngine::mediaTime(int deck) const
{
	if (!validDeck(deck))
		return 0;
	const Deck &d = m_decks[deck];
	if (d.pending.active)
		return d.pending.alignEnd ? 0 : d.pending.seekMs;
	if (d.stepPos >= 0.0 && d.stepTimer.isValid() && d.stepTimer.elapsed() < kStepMemoryMs)
		return std::llround(d.stepPos);
	SourcePtr src = peekSource(d);
	return src ? obs_source_media_get_time(src.get()) : 0;
}

qint64 ReplayEngine::mediaDuration(int deck) const
{
	if (!validDeck(deck))
		return 0;
	SourcePtr src = peekSource(m_decks[deck]);
	return src ? obs_source_media_get_duration(src.get()) : 0;
}

obs_media_state ReplayEngine::mediaState(int deck) const
{
	if (!validDeck(deck))
		return OBS_MEDIA_STATE_NONE;
	SourcePtr src = peekSource(m_decks[deck]);
	return src ? obs_source_media_get_state(src.get()) : OBS_MEDIA_STATE_NONE;
}

bool ReplayEngine::isLoading(int deck) const
{
	return validDeck(deck) && m_decks[deck].pending.active;
}

double ReplayEngine::frameDurationMs() const
{
	obs_video_info ovi{};
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0)
		return 1000.0 * double(ovi.fps_den) / double(ovi.fps_num);
	return 1000.0 / 30.0;
}

} // namespace bbr
