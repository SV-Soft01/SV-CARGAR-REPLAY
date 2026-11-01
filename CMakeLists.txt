cmake_minimum_required(VERSION 3.28...3.31)

project(
  obs-basketball-replay
  VERSION 1.0.0
  DESCRIPTION "Replay A/B multi-cámara para baloncesto (OBS Studio 30+)"
  LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "obs-basketball-replay solo soporta compilación de 64 bits (x64).")
endif()

# ---------------------------------------------------------------------------
# Estándar C++ y Qt
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

# ---------------------------------------------------------------------------
# Dependencias
#   - libobs / obs-frontend-api: instalados con el componente "Development"
#     del árbol de obs-studio (ver .github/workflows/build.yml).
#   - Qt6: paquete precompilado obs-deps-qt6 (el mismo que usa OBS).
#   Pásalos con -DCMAKE_PREFIX_PATH="ruta/obs-sdk;ruta/obs-deps;ruta/obs-deps-qt6"
# ---------------------------------------------------------------------------
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets Network)

set(PLUGIN_SOURCES
    src/plugin-main.cpp
    src/replay-types.hpp
    src/clip-scanner.hpp
    src/clip-scanner.cpp
    src/replay-engine.hpp
    src/replay-engine.cpp
    src/replay-dock.hpp
    src/replay-dock.cpp
    src/replay-web-server.hpp
    src/replay-web-server.cpp)

add_library(${PROJECT_NAME} MODULE ${PLUGIN_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE PLUGIN_NAME="${PROJECT_NAME}"
          PLUGIN_VERSION="${PROJECT_VERSION}"
          $<$<PLATFORM_ID:Windows>:NOMINMAX>
          $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
          $<$<PLATFORM_ID:Windows>:UNICODE>
          $<$<PLATFORM_ID:Windows>:_UNICODE>)

if(MSVC)
  # /utf-8: las cadenas de la interfaz están en español (UTF-8).
  # /Zc:__cplusplus y /permissive- son requeridos por Qt 6.
  target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /W3 /MP /permissive- /Zc:__cplusplus /Zc:preprocessor)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets Qt6::Network)

set_target_properties(
  ${PROJECT_NAME}
  PROPERTIES PREFIX ""
             OUTPUT_NAME "obs-basketball-replay"
             AUTOMOC ON)

if(MSVC)
  set_target_properties(${PROJECT_NAME} PROPERTIES COMPILE_PDB_NAME "${PROJECT_NAME}")
endif()

# ---------------------------------------------------------------------------
# Instalación: estructura compatible con la carpeta de OBS Studio
#   <OBS>/obs-plugins/64bit/obs-basketball-replay.dll
# ---------------------------------------------------------------------------
install(
  TARGETS ${PROJECT_NAME}
  LIBRARY DESTINATION obs-plugins/64bit
  RUNTIME DESTINATION obs-plugins/64bit)

if(MSVC)
  install(
    FILES $<TARGET_PDB_FILE:${PROJECT_NAME}>
    DESTINATION obs-plugins/64bit
    OPTIONAL)
endif()
