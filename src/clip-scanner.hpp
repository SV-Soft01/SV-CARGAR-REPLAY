#pragma once

// ---------------------------------------------------------------------------
// Escáner de clips en segundo plano + persistencia de etiquetas
//
//  - ClipScanner vive en el hilo de UI y delega el trabajo de disco a
//    ScanWorker, que corre en un QThread propio de baja prioridad.
//    Nunca se toca el disco desde el hilo de UI ni desde el hilo de render.
//  - TagStore guarda las etiquetas en un JSON junto a los videos.
// ---------------------------------------------------------------------------

#include "replay-types.hpp"

#include <QByteArray>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QThread>
#include <QTimer>

namespace bbr {

class ScanWorker : public QObject {
	Q_OBJECT

public slots:
	void scan(const QString &folder, bool force);

signals:
	void finished(const QString &folder, bool changed, const bbr::ClipList &clips);

private:
	QString m_lastFolder;
	QByteArray m_lastSignature;
};

class ClipScanner : public QObject {
	Q_OBJECT

public:
	explicit ClipScanner(QObject *parent = nullptr);
	~ClipScanner() override;

	void setFolder(const QString &folder);
	QString folder() const { return m_folder; }

	void start();
	void stop();

public slots:
	void requestScan(bool force = false);

signals:
	void clipsUpdated(const bbr::ClipList &clips);
	void scanRequested(const QString &folder, bool force); // interno -> worker

private:
	void onWorkerFinished(const QString &folder, bool changed, const bbr::ClipList &clips);
	void updateWatcher();

	QThread m_thread;
	ScanWorker *m_worker = nullptr;
	QTimer m_pollTimer;
	QTimer m_debounce;
	QFileSystemWatcher m_watcher;
	QString m_folder;
	bool m_busy = false;
	bool m_again = false;
	bool m_againForce = false;
	bool m_stopped = false;
};

// ---------------------------------------------------------------------------
// Persistencia de etiquetas (basketball_replay_tags.json en la carpeta de clips)
// ---------------------------------------------------------------------------
class TagStore {
public:
	void load(const QString &folder);
	bool save() const;

	ClipTags get(const QString &fileName) const;
	void set(const QString &fileName, const ClipTags &tags);

	QString path() const { return m_path; }

private:
	QString m_path;
	QHash<QString, ClipTags> m_tags;
};

} // namespace bbr
