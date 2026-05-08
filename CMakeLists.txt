cmake_minimum_required(VERSION 3.16)

project(spotify_recorder_qt LANGUAGES CXX)

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
  recorder_controller.cpp
  qt_main.cpp
  qt_recorder_window.cpp
)

target_link_libraries(spotify-recorder-qt PRIVATE ${QT_PACKAGE}::Widgets)

if (WIN32)
  set_target_properties(spotify-recorder-qt PROPERTIES WIN32_EXECUTABLE ON)
endif()
