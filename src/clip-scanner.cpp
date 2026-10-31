#include "clip-scanner.hpp"

#include <util/base.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>

namespace bbr {

namespace {

constexpr int kPollIntervalMs = 2000;
constexpr int kWatcherDebounceMs = 400;
constexpr qint64 kMinFileAgeMs = 1500;      // archivo aún escribiéndose (replay buffer)
constexpr qint64 kGroupToleranceMs = 4000;  // clips de distintas cámaras guardados "a la vez"

const QStringList &videoFilters()
{
	static const QStringList filters = {QStringLiteral("*.mkv"), QStringLiteral("*.mp4"), QStringLiteral("*.mov"),
					    QStringLiteral("*.flv"), QStringLiteral("*.ts"),  QStringLiteral("*.m4v"),
					    QStringLiteral("*.webm")};
	return filters;
}

QString formatCameraLabel(const QString &raw)
{
	QString s = raw.trimmed();
	// Chequear si es camara1, cam2, etc.
	static const QRegularExpression reCamNum(
		QStringLiteral(R"(^(?:cam|camara|camera|cámara)[ _\-]?(\d+)$)"),
		QRegularExpression::CaseInsensitiveOption);
	QRegularExpressionMatch mNum = reCamNum.match(s);
	if (mNum.hasMatch()) {
		return QStringLiteral("Cámara %1").arg(mNum.captured(1));
	}

	// Si empieza con camara_ / camera_
	static const QRegularExpression reCamPrefix(
		QStringLiteral(R"(^(?:camara|camera|cámara)[ _\-]+(.+)$)"),
		QRegularExpression::CaseInsensitiveOption);
	QRegularExpressionMatch mPref = reCamPrefix.match(s);
	if (mPref.hasMatch()) {
		s = mPref.captured(1).trimmed();
		QStringList words = s.split(QLatin1Char('_'), Qt::SkipEmptyParts);
		for (QString &w : words) {
			if (!w.isEmpty())
				w = w.at(0).toUpper() + w.mid(1).toLower();
		}
		return QStringLiteral("Cámara %1").arg(words.join(QLatin1Char(' ')));
	}

	// Formatear palabras separadas por _ o guión
	QStringList words = s.split(QRegularExpression(QStringLiteral("[_\\-]+")), Qt::SkipEmptyParts);
	for (QString &w : words) {
		if (!w.isEmpty())
			w = w.at(0).toUpper() + w.mid(1).toLower();
	}
	return words.join(QLatin1Char(' '));
}

void parseReplayFileInfo(const QFileInfo &fi, ClipInfo &c)
{
	const QString baseName = fi.completeBaseName();

	// -----------------------------------------------------------------------
	// Formato A: Multi-cámara independiente
	// Ejemplo: replay_0016_20261025_112431_camara_tongo
	//          replay_0016_20261025_112431_primera_base
	//          replay_0012_20261024_223102_camara1
	//          replay_0024_20261025_153331_primera_base
	// -----------------------------------------------------------------------
	static const QRegularExpression reMulti(
		QStringLiteral(R"(^replay_(\d+)_(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})_(.+)$)"),
		QRegularExpression::CaseInsensitiveOption);

	QRegularExpressionMatch mMulti = reMulti.match(baseName);
	if (mMulti.hasMatch()) {
		const int y = mMulti.captured(2).toInt();
		const int m = mMulti.captured(3).toInt();
		const int d = mMulti.captured(4).toInt();
		const int hh = mMulti.captured(5).toInt();
		const int mm = mMulti.captured(6).toInt();
		const int ss = mMulti.captured(7).toInt();
		const QDateTime dt(QDate(y, m, d), QTime(hh, mm, ss));
		c.eventTimeMs = dt.isValid() ? dt.toMSecsSinceEpoch() : fi.lastModified().toMSecsSinceEpoch();

		const QString rawCam = mMulti.captured(8);
		c.cameraKey = rawCam.toLower().trimmed();
		c.cameraName = formatCameraLabel(rawCam);
		c.isPrincipal = false;

		static const QRegularExpression numCheck(QStringLiteral(R"(\d+)"));
		QRegularExpressionMatch mNum = numCheck.match(rawCam);
		c.camera = mNum.hasMatch() ? mNum.captured(0).toInt() : 1;

		c.baseKey = QStringLiteral("replay_%1_%2%3%4_%5%6%7")
			.arg(mMulti.captured(1))
			.arg(mMulti.captured(2)).arg(mMulti.captured(3)).arg(mMulti.captured(4))
			.arg(mMulti.captured(5)).arg(mMulti.captured(6)).arg(mMulti.captured(7));
		return;
	}

	// -----------------------------------------------------------------------
	// Formato B: Buffer de repetición Principal de OBS
	// Ejemplo: 107 - Replay 2026-10-24 22-31-02
	//          111 - Replay 2026-10-25 11-24-32
	//          Replay 2026-10-24 22-31-02
	//          139 - 2026-10-25 12-52-05
	// -----------------------------------------------------------------------
	static const QRegularExpression reObs(
		QStringLiteral(R"(^(?:(\d+)\s*[-_]\s*)?(?:replay[-_ ]*)?(\d{4})[-_](\d{2})[-_](\d{2})[-_ ]+(\d{2})[-_](\d{2})[-_](\d{2})(?:[ _\-]+(.+))?$)"),
		QRegularExpression::CaseInsensitiveOption);

	QRegularExpressionMatch mObs = reObs.match(baseName);
	if (mObs.hasMatch()) {
		const int y = mObs.captured(2).toInt();
		const int m = mObs.captured(3).toInt();
		const int d = mObs.captured(4).toInt();
		const int hh = mObs.captured(5).toInt();
		const int mm = mObs.captured(6).toInt();
		const int ss = mObs.captured(7).toInt();
		const QDateTime dt(QDate(y, m, d), QTime(hh, mm, ss));
		c.eventTimeMs = dt.isValid() ? dt.toMSecsSinceEpoch() : fi.lastModified().toMSecsSinceEpoch();

		const QString suffix = mObs.captured(8).trimmed();
		if (suffix.isEmpty()) {
			c.isPrincipal = true;
			c.camera = 0;
			c.cameraKey = QStringLiteral("principal");
			c.cameraName = QStringLiteral("Principal");
		} else {
			c.isPrincipal = false;
			c.cameraKey = suffix.toLower();
			c.cameraName = formatCameraLabel(suffix);
			static const QRegularExpression numCheck(QStringLiteral(R"(\d+)"));
			QRegularExpressionMatch mNum = numCheck.match(suffix);
			c.camera = mNum.hasMatch() ? mNum.captured(0).toInt() : 1;
		}

		c.baseKey = QStringLiteral("%1-%2-%3_%4-%5-%6")
			.arg(mObs.captured(2)).arg(mObs.captured(3)).arg(mObs.captured(4))
			.arg(mObs.captured(5)).arg(mObs.captured(6)).arg(mObs.captured(7));
		return;
	}

	// -----------------------------------------------------------------------
	// Formato C: Genérico / Fallback
	// -----------------------------------------------------------------------
	c.eventTimeMs = fi.lastModified().toMSecsSinceEpoch();
	if (baseName.contains(QStringLiteral("replay"), Qt::CaseInsensitive) &&
	    !baseName.contains(QStringLiteral("cam"), Qt::CaseInsensitive)) {
		c.isPrincipal = true;
		c.camera = 0;
		c.cameraKey = QStringLiteral("principal");
		c.cameraName = QStringLiteral("Principal");
	} else {
		c.isPrincipal = false;
		c.camera = 1;
		c.cameraKey = QStringLiteral("cam1");
		c.cameraName = QStringLiteral("Cámara 1");
	}
	c.baseKey = baseName;
}

struct Group {
	QString key;
	qint64 anchorMs = 0;
	QStringList cameraKeys;
};

void assignGroups(ClipList &clips)
{
	constexpr qint64 kMaxGroupDeltaMs = 4000; // Tolerancia de hasta 4s para clips descargados juntos

	QVector<Group> groups;
	for (ClipInfo &c : clips) {
		const qint64 t = (c.eventTimeMs > 0) ? c.eventTimeMs : c.mtimeMs;
		int gi = -1;

		// Buscar un grupo existente dentro de la tolerancia temporal que NO contenga ya esta cámara
		for (int i = int(groups.size()) - 1; i >= 0; --i) {
			const Group &g = groups[i];
			if (std::abs(t - g.anchorMs) <= kMaxGroupDeltaMs) {
				if (!g.cameraKeys.contains(c.cameraKey)) {
					gi = i;
					break;
				}
			}
		}

		if (gi < 0) {
			Group g;
			g.key = c.filePath;
			g.anchorMs = t;
			g.cameraKeys.push_back(c.cameraKey);
			groups.push_back(g);
			gi = int(groups.size()) - 1;
		} else {
			groups[gi].cameraKeys.push_back(c.cameraKey);
		}

		c.groupKey = groups[gi].key;
	}
}

} // namespace

// ===========================================================================
// ScanWorker (hilo de escaneo)
// ===========================================================================
void ScanWorker::scan(const QString &folder, bool force)
{
	if (force || folder != m_lastFolder)
		m_lastSignature.clear();
	m_lastFolder = folder;

	ClipList clips;
	QDir dir(folder);

	if (!folder.isEmpty() && dir.exists()) {
		const QFileInfoList entries =
			dir.entryInfoList(videoFilters(), QDir::Files | QDir::Readable | QDir::NoDotAndDotDot);
		const qint64 now = QDateTime::currentMSecsSinceEpoch();

		clips.reserve(entries.size());
		for (const QFileInfo &fi : entries) {
			if (fi.size() <= 0)
				continue;
			const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
			if (now - mtime < kMinFileAgeMs)
				continue; // todavía escribiéndose; el próximo ciclo lo recogerá

			ClipInfo c;
			c.filePath = fi.absoluteFilePath();
			c.fileName = fi.fileName();
			c.mtimeMs = mtime;
			c.sizeBytes = fi.size();
			parseReplayFileInfo(fi, c);
			clips.push_back(std::move(c));
		}
	}

	std::sort(clips.begin(), clips.end(), [](const ClipInfo &a, const ClipInfo &b) {
		const qint64 ta = (a.eventTimeMs > 0) ? a.eventTimeMs : a.mtimeMs;
		const qint64 tb = (b.eventTimeMs > 0) ? b.eventTimeMs : b.mtimeMs;
		if (ta != tb)
			return ta < tb;
		if (a.isPrincipal != b.isPrincipal)
			return a.isPrincipal; // Principal primero en el mismo segundo
		if (a.camera != b.camera)
			return a.camera < b.camera;
		return a.fileName < b.fileName;
	});

	for (int i = 0; i < clips.size(); ++i)
		clips[i].number = i + 1;

	assignGroups(clips);

	QCryptographicHash hash(QCryptographicHash::Sha1);
	hash.addData(folder.toUtf8());
	for (const ClipInfo &c : clips) {
		hash.addData(c.filePath.toUtf8());
		hash.addData(QByteArray::number(c.sizeBytes));
		hash.addData(QByteArray::number(c.mtimeMs));
	}
	const QByteArray signature = hash.result();
	const bool changed = signature != m_lastSignature;
	m_lastSignature = signature;

	emit finished(folder, changed, clips);
}

// ===========================================================================
// ClipScanner (hilo de UI)
// ===========================================================================
ClipScanner::ClipScanner(QObject *parent) : QObject(parent)
{
	qRegisterMetaType<bbr::ClipInfo>("bbr::ClipInfo");
	qRegisterMetaType<bbr::ClipList>("bbr::ClipList");

	m_worker = new ScanWorker;
	m_worker->moveToThread(&m_thread);
	m_thread.setObjectName(QStringLiteral("bbr-clip-scanner"));

	connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
	connect(this, &ClipScanner::scanRequested, m_worker, &ScanWorker::scan, Qt::QueuedConnection);
	connect(m_worker, &ScanWorker::finished, this, &ClipScanner::onWorkerFinished, Qt::QueuedConnection);

	m_pollTimer.setInterval(kPollIntervalMs);
	connect(&m_pollTimer, &QTimer::timeout, this, [this]() { requestScan(false); });

	m_debounce.setSingleShot(true);
	m_debounce.setInterval(kWatcherDebounceMs);
	connect(&m_debounce, &QTimer::timeout, this, [this]() { requestScan(false); });

	connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this]() { m_debounce.start(); });
}

ClipScanner::~ClipScanner()
{
	stop();
}

void ClipScanner::start()
{
	if (m_stopped)
		return;
	if (!m_thread.isRunning())
		m_thread.start(QThread::LowPriority);
	m_pollTimer.start();
	requestScan(true);
}

void ClipScanner::stop()
{
	if (m_stopped)
		return;
	m_stopped = true;
	m_pollTimer.stop();
	m_debounce.stop();
	if (m_thread.isRunning()) {
		m_thread.quit();
		m_thread.wait(3000);
	}
}

void ClipScanner::setFolder(const QString &folder)
{
	const QString clean = folder.isEmpty() ? QString() : QDir::cleanPath(folder);
	if (clean == m_folder)
		return;
	m_folder = clean;
	updateWatcher();
	requestScan(true);
}

void ClipScanner::updateWatcher()
{
	const QStringList dirs = m_watcher.directories();
	if (!dirs.isEmpty())
		m_watcher.removePaths(dirs);
	if (!m_folder.isEmpty() && QDir(m_folder).exists())
		m_watcher.addPath(m_folder);
}

void ClipScanner::requestScan(bool force)
{
	if (m_stopped || !m_thread.isRunning())
		return;
	if (m_busy) {
		m_again = true;
		m_againForce = m_againForce || force;
		return;
	}
	if (m_watcher.directories().isEmpty() && !m_folder.isEmpty() && QDir(m_folder).exists())
		updateWatcher(); // la carpeta apareció después
	m_busy = true;
	emit scanRequested(m_folder, force);
}

void ClipScanner::onWorkerFinished(const QString &folder, bool changed, const bbr::ClipList &clips)
{
	m_busy = false;
	if (folder == m_folder && changed)
		emit clipsUpdated(clips);

	if (m_again) {
		const bool f = m_againForce;
		m_again = false;
		m_againForce = false;
		requestScan(f);
	}
}

// ===========================================================================
// TagStore
// ===========================================================================
void TagStore::load(const QString &folder)
{
	m_tags.clear();
	m_path.clear();
	if (folder.isEmpty())
		return;

	m_path = QDir(folder).filePath(QString::fromUtf8(kTagsFileName));

	QFile f(m_path);
	if (!f.exists() || !f.open(QIODevice::ReadOnly))
		return;

	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		blog(LOG_WARNING, "%s No se pudo leer %s: %s", kLogPrefix, m_path.toUtf8().constData(),
		     err.errorString().toUtf8().constData());
		return;
	}

	const QJsonObject clips = doc.object().value(QStringLiteral("clips")).toObject();
	for (auto it = clips.begin(); it != clips.end(); ++it) {
		const QJsonObject o = it.value().toObject();
		ClipTags t;
		const QString team = o.value(QStringLiteral("team")).toString();
		if (team == QLatin1String("C"))
			t.team = Team::Home;
		else if (team == QLatin1String("V"))
			t.team = Team::Away;

		const QJsonArray plays = o.value(QStringLiteral("plays")).toArray();
		for (const QJsonValue &p : plays) {
			const QString s = p.toString();
			if (s == QLatin1String("3PT"))
				t.set(Play3PT, true);
			else if (s == QLatin1String("TOP"))
				t.set(PlayTop, true);
			else if (s == QLatin1String("VAR"))
				t.set(PlayVar, true);
		}
		if (!t.isEmpty())
			m_tags.insert(it.key(), t);
	}
	blog(LOG_INFO, "%s Etiquetas cargadas: %d (%s)", kLogPrefix, int(m_tags.size()), m_path.toUtf8().constData());
}

bool TagStore::save() const
{
	if (m_path.isEmpty())
		return false;

	QJsonObject clips;
	for (auto it = m_tags.cbegin(); it != m_tags.cend(); ++it) {
		const ClipTags &t = it.value();
		QJsonObject o;
		o.insert(QStringLiteral("team"), t.team == Team::Home   ? QStringLiteral("C")
						 : t.team == Team::Away ? QStringLiteral("V")
									: QString());
		QJsonArray plays;
		if (t.has(Play3PT))
			plays.append(QStringLiteral("3PT"));
		if (t.has(PlayTop))
			plays.append(QStringLiteral("TOP"));
		if (t.has(PlayVar))
			plays.append(QStringLiteral("VAR"));
		o.insert(QStringLiteral("plays"), plays);
		clips.insert(it.key(), o);
	}

	QJsonObject root;
	root.insert(QStringLiteral("app"), QStringLiteral("obs-basketball-replay"));
	root.insert(QStringLiteral("version"), 1);
	root.insert(QStringLiteral("clips"), clips);

	QSaveFile f(m_path);
	f.setDirectWriteFallback(true); // carpetas sincronizadas (OneDrive) pueden bloquear el rename
	if (!f.open(QIODevice::WriteOnly)) {
		blog(LOG_WARNING, "%s No se pudo escribir %s", kLogPrefix, m_path.toUtf8().constData());
		return false;
	}
	f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	return f.commit();
}

ClipTags TagStore::get(const QString &fileName) const
{
	return m_tags.value(fileName);
}

void TagStore::set(const QString &fileName, const ClipTags &tags)
{
	if (tags.isEmpty())
		m_tags.remove(fileName);
	else
		m_tags.insert(fileName, tags);
}

} // namespace bbr
