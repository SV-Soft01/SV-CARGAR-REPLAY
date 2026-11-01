#include "replay-dock.hpp"
#include "replay-types.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/base.h>

#include <QAction>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *kDockId = "obs_basketball_replay_dock";

QPointer<bbr::ReplayDock> s_dock;

void onFrontendEvent(enum obs_frontend_event event, void * /*private_data*/)
{
	if (!s_dock)
		return;

	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		s_dock->init();
		// Asegurar que sea visible tras cargar la interfaz
		s_dock->setVisible(true);
		s_dock->show();
	} else if (event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED) {
		s_dock->refreshSceneList();
	}
}

} // namespace

MODULE_EXPORT bool obs_module_load(void)
{
	blog(LOG_INFO, "%s Cargando obs-basketball-replay v%s para OBS Studio...", bbr::kLogPrefix, PLUGIN_VERSION);

	QMainWindow *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		blog(LOG_ERROR, "%s No se pudo obtener la ventana principal de OBS Studio (modo sin GUI?).", bbr::kLogPrefix);
		return false;
	}

	s_dock = new bbr::ReplayDock(mainWindow);
	obs_frontend_add_custom_qdock(kDockId, s_dock);
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);

	// Hacer visible el dock
	s_dock->setVisible(true);
	s_dock->show();

	// 1. Agregar al menú Herramientas (Tools)
	QAction *toolsAction = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction("Basketball Replay A/B"));
	if (toolsAction) {
		QObject::connect(toolsAction, &QAction::triggered, [dock = s_dock]() {
			if (dock) {
				dock->setVisible(!dock->isVisible());
				if (dock->isVisible()) {
					dock->raise();
					dock->activateWindow();
				}
			}
		});
	}

	// 2. Agregar al menú Paneles (Docks) nativo de OBS
	if (mainWindow->menuBar()) {
		QMenu *docksMenu = nullptr;
		for (QAction *action : mainWindow->menuBar()->actions()) {
			if (action->menu()) {
				QString title = action->text().toLower();
				if (title.contains("dock") || title.contains("panel") || title.contains("paneles")) {
					docksMenu = action->menu();
					break;
				}
			}
		}
		if (docksMenu) {
			QAction *dockToggleAct = s_dock->toggleViewAction();
			dockToggleAct->setText(QStringLiteral("Basketball Replay A/B"));
			docksMenu->addAction(dockToggleAct);
		}
	}

	blog(LOG_INFO, "%s Plugin cargado exitosamente.", bbr::kLogPrefix);
	return true;
}

MODULE_EXPORT void obs_module_unload(void)
{
	blog(LOG_INFO, "%s Descargando obs-basketball-replay...", bbr::kLogPrefix);
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	obs_frontend_remove_dock(kDockId);
	if (s_dock) {
		delete s_dock;
		s_dock = nullptr;
	}
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Basketball Replay A/B Deck";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Plugin nativo de repetición instantánea A/B y multi-cámara para transmisiones de baloncesto.";
}
