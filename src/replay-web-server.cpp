#include "replay-web-server.hpp"
#include "clip-scanner.hpp"
#include "replay-engine.hpp"

#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QUrl>
#include <QUrlQuery>

namespace bbr {

namespace {

const char *kHtmlTemplate = R"rawhtml(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover">
<title>Replay Remote</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background: #0f1216;
    color: #e6edf3;
    user-select: none;
    -webkit-user-select: none;
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    overflow-x: hidden;
  }
  header {
    background: #161b22;
    padding: 8px 12px;
    display: flex;
    justify-content: space-between;
    align-items: center;
    border-bottom: 1px solid #30363d;
  }
  .app-title { font-size: 0.95rem; font-weight: 700; letter-spacing: 0.5px; color: #58a6ff; display: flex; align-items: center; gap: 6px; }
  .status-badge {
    font-size: 0.75rem;
    padding: 3px 8px;
    border-radius: 12px;
    background: #238636;
    color: #fff;
    font-weight: 600;
  }
  .status-badge.offline { background: #da3633; }

  /* Contenedor Adaptable */
  .container {
    display: flex;
    flex-direction: column;
    flex: 1;
    padding: 8px;
    gap: 8px;
  }

  /* Modo Horizontal (Landscape) */
  @media (orientation: landscape) and (min-width: 500px) {
    .container {
      flex-direction: row;
      height: calc(100vh - 46px);
      overflow: hidden;
    }
    .col-clips {
      flex: 1.1;
      display: flex;
      flex-direction: column;
      overflow: hidden;
    }
    .col-controls {
      flex: 0.9;
      display: flex;
      flex-direction: column;
      overflow-y: auto;
      gap: 8px;
    }
  }

  /* Botones base */
  button {
    font-family: inherit;
    border: none;
    cursor: pointer;
    border-radius: 8px;
    font-weight: 700;
    transition: transform 0.08s ease, filter 0.1s ease;
    touch-action: manipulation;
  }
  button:active { transform: scale(0.96); filter: brightness(1.2); }

  /* Selector de Decks */
  .deck-switcher {
    display: flex;
    gap: 6px;
    background: #161b22;
    padding: 4px;
    border-radius: 8px;
    border: 1px solid #30363d;
  }
  .deck-btn {
    flex: 1;
    padding: 8px 4px;
    font-size: 0.85rem;
    background: transparent;
    color: #8b949e;
  }
  .deck-btn.active {
    background: #1f6feb;
    color: #fff;
    box-shadow: 0 2px 6px rgba(31,111,235,0.4);
  }
  .deck-btn.on-air::after {
    content: " • AIRE";
    color: #f85149;
    font-weight: 800;
  }

  /* Botones Gigantes de Emisión */
  .emission-bar {
    display: flex;
    gap: 8px;
  }
  .btn-take {
    flex: 2;
    padding: 14px 8px;
    font-size: 1.1rem;
    background: #238636;
    color: #fff;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    box-shadow: 0 3px 8px rgba(35,134,54,0.4);
  }
  .btn-live {
    flex: 1;
    padding: 14px 6px;
    font-size: 0.85rem;
    background: #da3633;
    color: #fff;
    box-shadow: 0 3px 8px rgba(218,54,51,0.4);
  }

  /* Ángulos Multi-Cámara */
  .angles-panel {
    background: #161b22;
    padding: 6px;
    border-radius: 8px;
    border: 1px solid #30363d;
  }
  .section-label {
    font-size: 0.72rem;
    font-weight: 700;
    color: #8b949e;
    text-transform: uppercase;
    margin-bottom: 4px;
  }
  .angles-grid {
    display: flex;
    flex-wrap: wrap;
    gap: 4px;
  }
  .angle-btn {
    flex: 1 1 calc(50% - 4px);
    padding: 7px 4px;
    font-size: 0.8rem;
    background: #21262d;
    color: #c9d1d9;
    border: 1px solid #30363d;
  }
  .angle-btn.active {
    background: #238636;
    color: #fff;
    border-color: #2ea043;
  }

  /* Panel de Velocidad y Transporte */
  .control-panel {
    background: #161b22;
    padding: 8px;
    border-radius: 8px;
    border: 1px solid #30363d;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .transport-row, .speed-row {
    display: flex;
    gap: 6px;
  }
  .transport-btn {
    flex: 1;
    padding: 9px 4px;
    font-size: 0.8rem;
    background: #21262d;
    color: #c9d1d9;
    border: 1px solid #30363d;
  }
  .btn-play {
    flex: 1.4;
    background: #1f6feb;
    color: #fff;
  }
  .btn-play.playing {
    background: #d29922;
  }
  .speed-btn {
    flex: 1;
    padding: 8px 2px;
    font-size: 0.8rem;
    background: #21262d;
    color: #8b949e;
    border: 1px solid #30363d;
  }
  .speed-btn.active {
    background: #388bfd;
    color: #fff;
    font-weight: 800;
  }

  /* Info del Clip Activo */
  .clip-info-card {
    background: #161b22;
    padding: 8px 10px;
    border-radius: 8px;
    border: 1px solid #30363d;
    display: flex;
    justify-content: space-between;
    align-items: center;
  }
  .clip-name { font-size: 0.85rem; font-weight: 700; color: #f0f6fc; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 65%; }
  .clip-time { font-family: monospace; font-size: 0.82rem; color: #58a6ff; font-weight: 700; }

  /* Lista de Clips / Jugadas */
  .clips-panel {
    background: #161b22;
    border-radius: 8px;
    border: 1px solid #30363d;
    display: flex;
    flex-direction: column;
    flex: 1;
    overflow: hidden;
  }
  .clips-header {
    padding: 6px 10px;
    font-size: 0.75rem;
    font-weight: 700;
    color: #8b949e;
    border-bottom: 1px solid #30363d;
    display: flex;
    justify-content: space-between;
  }
  .clips-list {
    overflow-y: auto;
    -webkit-overflow-scrolling: touch;
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 2px;
    padding: 4px;
    max-height: 55vh;
  }
  @media (orientation: landscape) and (min-width: 500px) {
    .clips-list { max-height: none; }
  }
  .clip-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 8px 10px;
    background: #0d1117;
    border-radius: 6px;
    border: 1px solid #21262d;
    font-size: 0.82rem;
  }
  .clip-item:active { background: #1c2128; }
  .clip-item.active-preview { border-color: #f0883e; background: #261b11; }
  .clip-item.active-air { border-color: #f85149; background: #281014; }
  .clip-title { font-weight: 600; color: #c9d1d9; display: flex; align-items: center; gap: 6px; }
  .badge {
    font-size: 0.68rem;
    font-weight: 800;
    padding: 1px 5px;
    border-radius: 4px;
    background: #30363d;
    color: #e6edf3;
  }
  .badge.principal { background: #1f6feb; }
  .badge.pt3 { background: #8957e5; }
  .badge.top { background: #d29922; }
  .badge.var { background: #da3633; }
</style>
</head>
<body>

<header>
  <div class="app-title">🏀 Replay Remote</div>
  <div class="status-badge" id="connBadge">🟢 Conectado</div>
</header>

<div class="container">
  <!-- Columna Controles (En portrait va arriba, en landscape a la derecha) -->
  <div class="col-controls">
    <!-- Selector de Deck A/B -->
    <div class="deck-switcher">
      <button class="deck-btn active" id="btnDeckA" onclick="selectDeck(0)">DECK A (REPLAY)</button>
      <button class="deck-btn" id="btnDeckB" onclick="selectDeck(1)">DECK B (REPLAY 2)</button>
    </div>

    <!-- Botones de Emisión al Aire -->
    <div class="emission-bar">
      <button class="btn-take" onclick="doAction('take')">🚀 ENVIAR A PROGRAMA (TAKE)</button>
      <button class="btn-live" onclick="doAction('return_live')">🔴 EN VIVO</button>
    </div>

    <!-- Clip Info -->
    <div class="clip-info-card">
      <div class="clip-name" id="currentClipName">Sin clip cargado</div>
      <div class="clip-time" id="currentClipTime">00:00 / 00:00</div>
    </div>

    <!-- Ángulos Multi-Cámara -->
    <div class="angles-panel" id="anglesBox" style="display:none;">
      <div class="section-label">Ángulos Sincronizados</div>
      <div class="angles-grid" id="anglesList"></div>
    </div>

    <!-- Velocidad y Transporte -->
    <div class="control-panel">
      <div class="section-label">Velocidad de Reproducción</div>
      <div class="speed-row">
        <button class="speed-btn" id="spd25" onclick="setSpeed(25)">25%</button>
        <button class="speed-btn" id="spd50" onclick="setSpeed(50)">50%</button>
        <button class="speed-btn" id="spd75" onclick="setSpeed(75)">75%</button>
        <button class="speed-btn active" id="spd100" onclick="setSpeed(100)">100%</button>
      </div>
      <div class="transport-row">
        <button class="transport-btn" onclick="doAction('step_back')">[-1F]</button>
        <button class="transport-btn btn-play" id="btnPlayPause" onclick="doAction('play_pause')">PLAY</button>
        <button class="transport-btn" onclick="doAction('step_fwd')">[+1F]</button>
        <button class="transport-btn" onclick="doAction('stop')">STOP</button>
      </div>
    </div>
  </div>

  <!-- Columna Lista de Jugadas (En portrait va abajo, en landscape a la izquierda) -->
  <div class="col-clips">
    <div class="clips-panel">
      <div class="clips-header">
        <span>JUGADAS RECIENTES</span>
        <span id="clipCount">0 clips</span>
      </div>
      <div class="clips-list" id="clipsList">
        <div style="padding: 12px; color: #8b949e; text-align: center;">Cargando lista de repeticiones...</div>
      </div>
    </div>
  </div>
</div>

<script>
  let activeDeck = 0;
  let lastData = null;

  async function fetchStatus() {
    try {
      const res = await fetch('/api/status');
      if (!res.ok) throw new Error('HTTP Error');
      const data = await res.json();
      renderState(data);
      document.getElementById('connBadge').textContent = '🟢 Conectado';
      document.getElementById('connBadge').classList.remove('offline');
    } catch (e) {
      document.getElementById('connBadge').textContent = '🔴 Desconectado';
      document.getElementById('connBadge').classList.add('offline');
    }
  }

  function renderState(data) {
    lastData = data;
    activeDeck = data.activeDeck;

    // Actualizar botones de Deck
    const btnA = document.getElementById('btnDeckA');
    const btnB = document.getElementById('btnDeckB');
    btnA.className = 'deck-btn ' + (activeDeck === 0 ? 'active' : '') + (data.airDeck === 0 ? ' on-air' : '');
    btnB.className = 'deck-btn ' + (activeDeck === 1 ? 'active' : '') + (data.airDeck === 1 ? ' on-air' : '');

    // Actualizar Clip Info del Deck seleccionado
    const deckData = (activeDeck === 0) ? data.deckA : data.deckB;
    document.getElementById('currentClipName').textContent = deckData.clipTitle || 'Sin clip cargado';
    document.getElementById('currentClipTime').textContent = (deckData.timeStr || '00:00.000') + ' / ' + (deckData.durationStr || '00:00.000');

    // Botón Play / Pausa
    const btnPlay = document.getElementById('btnPlayPause');
    if (deckData.isPlaying) {
      btnPlay.textContent = 'PAUSA';
      btnPlay.classList.add('playing');
    } else {
      btnPlay.textContent = 'PLAY';
      btnPlay.classList.remove('playing');
    }

    // Botones de velocidad
    [25, 50, 75, 100].forEach(spd => {
      const b = document.getElementById('spd' + spd);
      if (b) b.classList.toggle('active', deckData.speed === spd);
    });

    // Ángulos
    const anglesBox = document.getElementById('anglesBox');
    const anglesList = document.getElementById('anglesList');
    if (data.angles && data.angles.length > 1) {
      anglesBox.style.display = 'block';
      anglesList.innerHTML = data.angles.map(a => `
        <button class="angle-btn ${a.selected ? 'active' : ''}" onclick="switchAngle('${encodeURIComponent(a.path)}')">
          ${a.label}
        </button>
      `).join('');
    } else {
      anglesBox.style.display = 'none';
    }

    // Lista de Clips
    const clipsList = document.getElementById('clipsList');
    document.getElementById('clipCount').textContent = (data.clips ? data.clips.length : 0) + ' clips';
    if (data.clips && data.clips.length > 0) {
      clipsList.innerHTML = data.clips.map(c => {
        let cls = 'clip-item';
        if (c.path === deckData.clipPath) cls += ' active-preview';
        return `
          <div class="${cls}" onclick="loadClip('${encodeURIComponent(c.path)}')">
            <div class="clip-title">
              <span>${c.displayName}</span>
              ${c.isPrincipal ? '<span class="badge principal">P</span>' : ''}
              ${c.badges ? `<span class="badge">${c.badges}</span>` : ''}
            </div>
            <span style="font-family:monospace; color:#8b949e; font-size:0.75rem;">${c.time}</span>
          </div>
        `;
      }).join('');
    } else {
      clipsList.innerHTML = '<div style="padding:12px; color:#8b949e; text-align:center;">No hay clips guardados</div>';
    }
  }

  async function doAction(act, extra = {}) {
    try {
      await fetch('/api/action', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: act, deck: activeDeck, ...extra })
      });
      fetchStatus();
    } catch(e) {}
  }

  function selectDeck(d) {
    activeDeck = d;
    doAction('select_deck', { deck: d });
  }

  function setSpeed(spd) {
    doAction('speed', { speed: spd });
  }

  function switchAngle(encodedPath) {
    doAction('switch_angle', { path: decodeURIComponent(encodedPath) });
  }

  function loadClip(encodedPath) {
    doAction('load_clip', { path: decodeURIComponent(encodedPath) });
  }

  // Intervalo de actualización en tiempo real (400ms)
  setInterval(fetchStatus, 400);
  fetchStatus();
</script>

</body>
</html>
)rawhtml";

} // namespace

ReplayWebServer::ReplayWebServer(ReplayEngine &engine, ClipScanner &scanner, TagStore &tagStore, QObject *parent)
	: QObject(parent), m_engine(engine), m_scanner(scanner), m_tagStore(tagStore)
{
}

ReplayWebServer::~ReplayWebServer()
{
	stop();
}

bool ReplayWebServer::start(quint16 port)
{
	if (m_server && m_server->isListening())
		return true;

	if (!m_server) {
		m_server = new QTcpServer(this);
		connect(m_server, &QTcpServer::newConnection, this, &ReplayWebServer::onNewConnection);
	}

	m_port = port;
	// Escuchar en todas las interfaces de red para permitir conexión de celular
	if (!m_server->listen(QHostAddress::AnyIPv4, m_port)) {
		// Reintentar en puerto alternativo si 8080 está ocupado
		if (!m_server->listen(QHostAddress::AnyIPv4, m_port + 1)) {
			return false;
		}
		m_port = m_port + 1;
	}

	const QString url = serverUrl();
	emit serverStarted(url);
	return true;
}

void ReplayWebServer::stop()
{
	if (m_server) {
		m_server->close();
		m_server->deleteLater();
		m_server = nullptr;
		emit serverStopped();
	}
}

bool ReplayWebServer::isRunning() const
{
	return m_server && m_server->isListening();
}

QString ReplayWebServer::localIpAddress() const
{
	const QList<QHostAddress> addrs = QNetworkInterface::allAddresses();
	for (const QHostAddress &addr : addrs) {
		if (addr.protocol() == QAbstractSocket::IPv4Protocol && !addr.isLoopback()) {
			const QString ip = addr.toString();
			// Priorizar redes locales estándar (192.168.x.x, 10.x.x.x, 172.x.x.x)
			if (ip.startsWith(QStringLiteral("192.168.")) || ip.startsWith(QStringLiteral("10.")) ||
			    ip.startsWith(QStringLiteral("172."))) {
				return ip;
			}
		}
	}
	return QStringLiteral("127.0.0.1");
}

QString ReplayWebServer::serverUrl() const
{
	return QStringLiteral("http://%1:%2").arg(localIpAddress()).arg(m_port);
}

void ReplayWebServer::onNewConnection()
{
	while (m_server && m_server->hasPendingConnections()) {
		QTcpSocket *socket = m_server->nextPendingConnection();
		connect(socket, &QTcpSocket::readyRead, this, &ReplayWebServer::onClientReadyRead);
		connect(socket, &QTcpSocket::disconnected, this, &ReplayWebServer::onClientDisconnected);
	}
}

void ReplayWebServer::onClientDisconnected()
{
	auto *socket = qobject_cast<QTcpSocket *>(sender());
	if (socket) {
		socket->deleteLater();
	}
}

void ReplayWebServer::onClientReadyRead()
{
	auto *socket = qobject_cast<QTcpSocket *>(sender());
	if (!socket)
		return;

	const QByteArray requestData = socket->readAll();
	handleHttpRequest(socket, requestData);
}

void ReplayWebServer::handleHttpRequest(QTcpSocket *socket, const QByteArray &requestData)
{
	const QString reqStr = QString::fromUtf8(requestData);
	const QStringList lines = reqStr.split(QStringLiteral("\r\n"));
	if (lines.isEmpty())
		return;

	const QStringList parts = lines.first().split(QLatin1Char(' '));
	if (parts.size() < 2)
		return;

	const QString method = parts[0].toUpper();
	const QString path = parts[1];

	if (method == QStringLiteral("GET")) {
		if (path == QStringLiteral("/") || path == QStringLiteral("/index.html")) {
			sendResponse(socket, 200, QStringLiteral("text/html; charset=utf-8"), buildHtmlPage());
			return;
		} else if (path.startsWith(QStringLiteral("/api/status"))) {
			sendResponse(socket, 200, QStringLiteral("application/json; charset=utf-8"), buildStatusJson());
			return;
		}
	} else if (method == QStringLiteral("POST") && path == QStringLiteral("/api/action")) {
		// Extraer cuerpo JSON tras la línea vacía
		const int bodyIdx = reqStr.indexOf(QStringLiteral("\r\n\r\n"));
		if (bodyIdx >= 0) {
			const QString body = reqStr.mid(bodyIdx + 4);
			const QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8());
			if (doc.isObject()) {
				const QJsonObject obj = doc.object();
				const QString action = obj.value(QStringLiteral("action")).toString();
				const int targetDeck = obj.value(QStringLiteral("deck")).toInt(m_activeDeck);
				m_activeDeck = (targetDeck == 1) ? 1 : 0;

				if (action == QStringLiteral("take")) {
					m_engine.takeToProgram(m_activeDeck);
				} else if (action == QStringLiteral("return_live")) {
					m_engine.returnToLive();
				} else if (action == QStringLiteral("play_pause")) {
					m_engine.togglePlayPause(m_activeDeck);
				} else if (action == QStringLiteral("stop")) {
					m_engine.stop(m_activeDeck);
				} else if (action == QStringLiteral("step_back")) {
					m_engine.stepFrame(m_activeDeck, -1);
				} else if (action == QStringLiteral("step_fwd")) {
					m_engine.stepFrame(m_activeDeck, 1);
				} else if (action == QStringLiteral("speed")) {
					const int spd = obj.value(QStringLiteral("speed")).toInt(100);
					m_engine.setSpeed(m_activeDeck, spd, true);
				} else if (action == QStringLiteral("select_deck")) {
					m_activeDeck = targetDeck;
					m_engine.sendToPreview(m_activeDeck);
				} else if (action == QStringLiteral("load_clip")) {
					const QString clipPath = obj.value(QStringLiteral("path")).toString();
					for (const ClipInfo &c : m_scanner.clips()) {
						if (c.filePath == clipPath) {
							m_engine.loadClip(m_activeDeck, c, false);
							m_engine.sendToPreview(m_activeDeck);
							break;
						}
					}
				} else if (action == QStringLiteral("switch_angle")) {
					const QString anglePath = obj.value(QStringLiteral("path")).toString();
					for (const ClipInfo &c : m_scanner.clips()) {
						if (c.filePath == anglePath) {
							m_engine.switchAngle(m_activeDeck, c);
							m_engine.sendToPreview(m_activeDeck);
							break;
						}
					}
				}
				emit clientActionTriggered(action);
			}
		}
		sendResponse(socket, 200, QStringLiteral("application/json"), "{\"status\":\"ok\"}");
		return;
	}

	sendResponse(socket, 404, QStringLiteral("text/plain"), "404 Not Found");
}

void ReplayWebServer::sendResponse(QTcpSocket *socket, int statusCode, const QString &contentType, const QByteArray &body)
{
	QByteArray resp;
	resp.append(QStringLiteral("HTTP/1.1 %1 OK\r\n").arg(statusCode).toUtf8());
	resp.append(QStringLiteral("Content-Type: %1\r\n").arg(contentType).toUtf8());
	resp.append(QStringLiteral("Content-Length: %1\r\n").arg(body.size()).toUtf8());
	resp.append("Access-Control-Allow-Origin: *\r\n");
	resp.append("Connection: close\r\n\r\n");
	resp.append(body);

	socket->write(resp);
	socket->flush();
	socket->disconnectFromHost();
}

QByteArray ReplayWebServer::buildHtmlPage() const
{
	return QByteArray(kHtmlTemplate);
}

QByteArray ReplayWebServer::buildStatusJson() const
{
	QJsonObject root;
	root[QStringLiteral("activeDeck")] = m_activeDeck;
	root[QStringLiteral("airDeck")] = m_engine.deckOnAir();

	auto packDeck = [this](int deckIdx) -> QJsonObject {
		QJsonObject o;
		const ClipInfo clip = m_engine.currentClip(deckIdx);
		const obs_media_state st = m_engine.mediaState(deckIdx);
		const qint64 t = m_engine.mediaTime(deckIdx);
		const qint64 dur = m_engine.mediaDuration(deckIdx);

		o[QStringLiteral("clipTitle")] = clip.isValid() ? clip.displayName() : QStringLiteral("Sin clip");
		o[QStringLiteral("clipPath")] = clip.filePath;
		o[QStringLiteral("camera")] = clip.cameraLabel();
		o[QStringLiteral("isPlaying")] = (st == OBS_MEDIA_STATE_PLAYING);
		o[QStringLiteral("speed")] = m_engine.speed(deckIdx);
		o[QStringLiteral("timeMs")] = t;
		o[QStringLiteral("durationMs")] = dur;
		o[QStringLiteral("timeStr")] = formatTimeMs(t);
		o[QStringLiteral("durationStr")] = formatTimeMs(dur);
		return o;
	};

	root[QStringLiteral("deckA")] = packDeck(0);
	root[QStringLiteral("deckB")] = packDeck(1);

	// Ángulos de la toma activa
	QJsonArray anglesArr;
	const ClipInfo current = m_engine.currentClip(m_activeDeck);
	if (current.isValid() && !current.groupKey.isEmpty()) {
		for (const ClipInfo &c : m_scanner.clips()) {
			if (c.groupKey == current.groupKey) {
				QJsonObject a;
				a[QStringLiteral("label")] = c.cameraLabel();
				a[QStringLiteral("path")] = c.filePath;
				a[QStringLiteral("selected")] = (c.filePath == current.filePath);
				anglesArr.append(a);
			}
		}
	}
	root[QStringLiteral("angles")] = anglesArr;

	// Lista de los últimos clips
	QJsonArray clipsArr;
	const ClipList allClips = m_scanner.clips();
	// Mostrar los clips más recientes primero (hasta 50)
	const qsizetype start = std::max<qsizetype>(0, allClips.size() - 50);
	for (qsizetype i = allClips.size() - 1; i >= start; --i) {
		const ClipInfo &c = allClips[i];
		const ClipTags tags = m_tagStore.get(c.fileName);

		QJsonObject co;
		co[QStringLiteral("number")] = c.number;
		co[QStringLiteral("displayName")] = c.displayName();
		co[QStringLiteral("path")] = c.filePath;
		co[QStringLiteral("camera")] = c.cameraLabel();
		co[QStringLiteral("isPrincipal")] = c.isPrincipal;
		const qint64 t = (c.eventTimeMs > 0) ? c.eventTimeMs : c.mtimeMs;
		co[QStringLiteral("time")] = QDateTime::fromMSecsSinceEpoch(t).toString(QStringLiteral("HH:mm:ss"));
		co[QStringLiteral("badges")] = tags.badges();
		clipsArr.append(co);
	}
	root[QStringLiteral("clips")] = clipsArr;

	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace bbr
