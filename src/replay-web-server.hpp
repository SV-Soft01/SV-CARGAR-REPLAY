#pragma once

#include "replay-types.hpp"

#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>

namespace bbr {

class ReplayEngine;
class ClipScanner;
class TagStore;

class ReplayWebServer : public QObject {
	Q_OBJECT

public:
	explicit ReplayWebServer(ReplayEngine &engine, ClipScanner &scanner, TagStore &tagStore,
				 QObject *parent = nullptr);
	~ReplayWebServer() override;

	bool start(quint16 port = 8080);
	void stop();
	bool isRunning() const;

	quint16 port() const { return m_port; }
	QString localIpAddress() const;
	QString serverUrl() const;

signals:
	void clientActionTriggered(const QString &action);
	void serverStarted(const QString &url);
	void serverStopped();

private slots:
	void onNewConnection();
	void onClientDisconnected();
	void onClientReadyRead();

private:
	void handleHttpRequest(QTcpSocket *socket, const QByteArray &requestData);
	void sendResponse(QTcpSocket *socket, int statusCode, const QString &contentType, const QByteArray &body);
	QByteArray buildHtmlPage() const;
	QByteArray buildStatusJson() const;

	ReplayEngine &m_engine;
	ClipScanner &m_scanner;
	TagStore &m_tagStore;

	QTcpServer *m_server = nullptr;
	quint16 m_port = 8080;
	int m_activeDeck = 0;
};

} // namespace bbr
