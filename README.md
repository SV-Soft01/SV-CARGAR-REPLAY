# obs-basketball-replay

Plugin nativo en C++20 y Qt 6 para **OBS Studio 30+** (especialmente optimizado para **OBS Studio 32.1.2 en Windows x64**). Diseñado específicamente para transmisiones profesionales de baloncesto y deportes en directo, reemplaza scripts de Lua antiguos, eliminando por completo los bloqueos, congelamientos de la interfaz gráfica y fallos de reproducción por ráfagas de cambio de velocidad en el demuxer de FFmpeg.

---

## Características Principales

1. **Arquitectura Double Deck A/B Roll ("REPLAY" y "REPLAY 2"):**
   - Manejo simultáneo de dos escenas y fuentes independientes.
   - Mientras una casilla está al aire en Programa, la otra puede ser cargada, inspeccionada o previsualizada en modo Estudio (Preview) sin interferir con la emisión.
   - Retorno automático configurable a la escena de cámara en vivo (`CAMARA EN VIVO` o la seleccionada en la interfaz) al finalizar la repetición.

2. **Gestión Multi-Cámara y Escaneo Asíncrono en Segundo Plano:**
   - Detección automática en un hilo de trabajo dedicado (`QThread` de baja prioridad) sin tocar el disco desde el hilo de interfaz ni el hilo de render.
   - Detección de sufijos de cámara (`_CAM1`, `_CAM2`, etc.) y clips del buffer estándar de OBS (sin sufijo = "Principal").
   - Numeración cronológica correlativa (`001 - ...`, `002 - ...`).
   - Filtros instantáneos por cámara en la interfaz.

3. **Control de Transporte Profesional y Revisión Arbitral:**
   - Play, Pausa, Stop (cue al inicio).
   - Avance y retroceso cuadro a cuadro exactos: `[-1 Frame]` y `[+1 Frame]` sin deriva temporal.
   - Barra de tiempo interactiva (*Scrubbing*) con indicador milisegundo a milisegundo.
   - Control de velocidad suave (0% a 100%) con botones rápidos (25%, 50%, 75%, 100%) protegidos con filtro anti-ráfaga (*debounce*) para evitar cuelgues del demuxer de FFmpeg.

4. **Sincronización Multi-Ángulo Instantánea:**
   - Al pulsar un ángulo alternativo de la misma jugada, salta automáticamente al mismo milisegundo exacto del clip.

5. **Etiquetado Deportivo (Baloncesto) y Persistencia JSON:**
   - Botones de marcado rápido:
     - Equipos: **[ C ]** (Casa) y **[ V ]** (Visitante).
     - Jugadas: **[ 3PT ]** (Triple), **[ TOP ]** (Jugada Destacada) y **[ VAR ]** (Revisión Arbitral).
   - Las etiquetas se guardan de forma instantánea y atómica en `basketball_replay_tags.json` en la misma carpeta de videos. Si OBS se cierra o reinicia, las marcas se conservan intactas.

6. **Modo Playlist / Resumen (Highlight Reel Continuo):**
   - Selección múltiple de clips en la tabla para reproducir en cadena continua.
   - **Manejo inteligente de Stinger:** El cambio entre clips se ejecuta internamente dentro de la misma fuente multimedia, **sin disparar la transición de escena ni el Stinger entre clip y clip**. El Stinger solo se activa al entrar a la repetición y al volver a vivo.
   - Botón **"Resumen Medio Tiempo (Playlist)"**: auto-selecciona cronológicamente todos los clips marcados como 3PT y TOP y los prepara en una casilla libre.

---

## Estructura del Proyecto

```text
├── .github/
│   └── workflows/
│       └── build.yml          # Pipeline CI para Windows x64 (genera el .dll)
├── CMakeLists.txt             # Configuración CMake para C++20, Qt6 y libobs
├── README.md
└── src/
    ├── clip-scanner.hpp/.cpp  # Escáner asíncrono en hilo secundario + persistencia JSON
    ├── plugin-main.cpp        # Punto de entrada de libobs (obs_module_load)
    ├── replay-dock.hpp/.cpp   # Panel QDockWidget nativo integrado con el tema de OBS
    ├── replay-engine.hpp/.cpp # Lógica Double Deck A/B, transporte y control FFmpeg
    └── replay-types.hpp       # Estructuras de datos compartidas y constantes
```

---

## Compilación y Descarga Automática con GitHub Actions

El repositorio incluye el flujo de integración continua en [`.github/workflows/build.yml`](file:///.github/workflows/build.yml):

1. Sube este proyecto a tu repositorio de GitHub.
2. Ve a la pestaña **Actions** en GitHub.
3. Ejecuta el workflow **Build obs-basketball-replay (Windows x64)** o haz un push / tag (`v1.0.0`).
4. Al terminar la ejecución, en la sección **Artifacts** podrás descargar el archivo `obs-basketball-replay-windows-x64-obs32.1.2.zip` que contiene el archivo `.dll` compilado y listo para usar.

---

## Instalación en OBS Studio (Windows x64)

1. Cierra OBS Studio.
2. Copia `obs-basketball-replay.dll` en la carpeta de plugins de OBS Studio:
   ```text
   C:\Program Files\obs-studio\obs-plugins\64bit\
   ```
   *(O si usas una versión portable de OBS, dentro de `<carpeta_obs>\obs-plugins\64bit\`)*.
3. Abre OBS Studio 30+ o 32+.
4. En el menú superior **Docks** (Paneles), verás **Basketball Replay A/B**.
5. Al abrir el panel, selecciona la carpeta donde guardas tus grabaciones o repeticiones y la escena de retorno en vivo.
