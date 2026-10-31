#pragma once

// ---------------------------------------------------------------------------
// Tipos compartidos del plugin obs-basketball-replay
// ---------------------------------------------------------------------------

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

namespace bbr {

inline constexpr int kDeckCount = 2;

// Deck A = "REPLAY", Deck B = "REPLAY 2"
inline constexpr const char *kDeckSceneNames[kDeckCount] = {"REPLAY", "REPLAY 2"};
inline constexpr const char *kDeckSourceNames[kDeckCount] = {"Replay Deck A", "Replay Deck B"};
inline constexpr const char *kDeckLabels[kDeckCount] = {"A", "B"};

inline constexpr const char *kDefaultLiveScene = "CAMARA EN VIVO";
inline constexpr const char *kTagsFileName = "basketball_replay_tags.json";
inline constexpr const char *kMediaSourceId = "ffmpeg_source";
inline constexpr const char *kLogPrefix = "[basketball-replay]";

// ---------------------------------------------------------------------------
// Etiquetas deportivas
// ---------------------------------------------------------------------------
enum class Team : int { None = 0, Home = 1, Away = 2 };

enum PlayFlag : uint32_t {
	PlayNone = 0,
	Play3PT = 1u << 0, // Triple
	PlayTop = 1u << 1, // Jugada destacada
	PlayVar = 1u << 2, // Revisión arbitral
};

struct ClipTags {
	Team team = Team::None;
	uint32_t plays = 0;

	bool has(PlayFlag f) const { return (plays & f) != 0; }
	void set(PlayFlag f, bool on) { plays = on ? (plays | f) : (plays & ~uint32_t(f)); }
	bool isEmpty() const { return team == Team::None && plays == 0; }

	QString badges() const
	{
		QStringList b;
		if (team == Team::Home)
			b << QStringLiteral("[C]");
		else if (team == Team::Away)
			b << QStringLiteral("[V]");
		if (has(Play3PT))
			b << QStringLiteral("[3PT]");
		if (has(PlayTop))
			b << QStringLiteral("[TOP]");
		if (has(PlayVar))
			b << QStringLiteral("[VAR]");
		return b.join(QLatin1Char(' '));
	}
};

// ---------------------------------------------------------------------------
// Clip de repetición detectado en disco
// ---------------------------------------------------------------------------
struct ClipInfo {
	QString filePath;  // ruta absoluta
	QString fileName;  // nombre con extensión (clave de etiquetas)
	QString baseKey;   // nombre sin extensión ni sufijo de cámara
	QString groupKey;  // identifica la "toma" multi-ángulo (ruta del primer clip del grupo)
	int camera = 0;    // 0 = Principal (buffer estándar de OBS), N = _CAMN
	qint64 mtimeMs = 0;
	qint64 sizeBytes = 0;
	int number = 0;    // numeración cronológica 1..N

	bool isValid() const { return !filePath.isEmpty(); }

	QString cameraLabel() const
	{
		return camera == 0 ? QStringLiteral("Principal") : QStringLiteral("CAM%1").arg(camera);
	}

	QString cameraLongLabel() const
	{
		return camera == 0 ? QStringLiteral("Principal") : QStringLiteral("Cámara %1").arg(camera);
	}

	QString displayName() const
	{
		const QString time = QDateTime::fromMSecsSinceEpoch(mtimeMs).toString(QStringLiteral("HH:mm:ss"));
		return QStringLiteral("%1 - %2 - %3").arg(number, 3, 10, QLatin1Char('0')).arg(cameraLabel(), time);
	}
};

using ClipList = QVector<ClipInfo>;

// Sincronización al cambiar de ángulo
enum class SyncMode : int {
	SameMillisecond = 0, // misma posición absoluta (ms)
	AlignEnd = 1,        // alinea por el final del clip (semántica del replay buffer)
};

// ---------------------------------------------------------------------------
// Configuración persistente del plugin
// ---------------------------------------------------------------------------
struct PluginSettings {
	QString replayFolder;
	QString liveScene = QString::fromUtf8(kDefaultLiveScene);
	bool autoReturn = true;
	bool autoDeck = true;
	bool hwDecode = false;
	SyncMode syncMode = SyncMode::SameMillisecond;
};

inline QString formatTimeMs(qint64 ms)
{
	if (ms < 0)
		ms = 0;
	const qint64 m = ms / 60000;
	const qint64 s = (ms / 1000) % 60;
	const qint64 r = ms % 1000;
	return QStringLiteral("%1:%2.%3")
		.arg(m, 2, 10, QLatin1Char('0'))
		.arg(s, 2, 10, QLatin1Char('0'))
		.arg(r, 3, 10, QLatin1Char('0'));
}

} // namespace bbr

Q_DECLARE_METATYPE(bbr::ClipInfo)
Q_DECLARE_METATYPE(bbr::ClipList)
