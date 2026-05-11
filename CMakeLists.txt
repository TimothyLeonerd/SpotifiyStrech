cmake_minimum_required(VERSION 3.16)

project(spotify_recorder_qt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 COMPONENTS Widgets QUIET)
if (Qt6_FOUND)
  set(QT_PACKAGE Qt6)
else()
  find_package(Qt5 COMPONENTS Widgets REQUIRED)
  set(QT_PACKAGE Qt5)
endif()

add_executable(spotify-recorder-qt
  core.c
  recorder_core.c
  playback_renderer.c
  qt_main.cpp
  qt_recorder_window.cpp
  third_party/rubberband/single/RubberBandSingle.cpp
)

target_include_directories(spotify-recorder-qt PRIVATE third_party/rubberband)
target_compile_definitions(spotify-recorder-qt PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

if (WIN32)
  target_sources(spotify-recorder-qt PRIVATE
    platform_windows.c
    windows_audio_backend.c
  )
  target_link_libraries(spotify-recorder-qt PRIVATE
    comctl32
    ole32
    uuid
    avrt
    mmdevapi
  )
endif()

target_link_libraries(spotify-recorder-qt PRIVATE ${QT_PACKAGE}::Widgets)

if (WIN32)
  set_target_properties(spotify-recorder-qt PROPERTIES WIN32_EXECUTABLE ON)
endif()
