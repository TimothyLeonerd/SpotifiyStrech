# Windows Port Notes

## Goal
Port the current app to a Qt Widgets UI while keeping the existing core behavior.

## Current Architecture
- `core.c` / `core.h`: portable logic
- `app.c`: current GTK3 + PulseAudio app path
- `qt_main.cpp` / `qt_recorder_window.cpp`: Qt Widgets UI shell
- `Makefile`: Linux build for GTK3, PulseAudio, Rubber Band
- `CMakeLists.txt`: Qt Widgets build path

## Portable Core
Already extracted into `core.c` / `core.h`:
- `AppMode`, `LoopState`, `LoopSnapshot`, `AudioBuffer`, `RenderIntent`
- transport decisions and plans
- UI-derived state snapshots
- status text generation
- loop math
- cursor/playhead math
- recording session reset

## Platform-Specific Layer
Still in `app.c`:
- GTK widget creation and callbacks
- Cairo waveform drawing
- PulseAudio capture/playback
- pointer grab / scrubbing events
- applying derived UI state to widgets

## App Behavior To Preserve
- record / stop / play-pause
- click-to-seek and scrubbing
- loop region creation and adjustment
- pitch-preserving speed changes via Rubber Band
- async render of playback buffer
- recording-time seek should survive into playback

## Likely Windows Replacements
- GTK UI -> Qt Widgets
- PulseAudio -> Windows audio API/backend
- pointer/input handling -> Windows event system
- drawing -> Qt painting

## Recommended Next Step
Keep `core.c` unchanged and move the recorder UI/host into Qt Widgets.

Suggested adapter split:
- audio backend: capture/playback/device selection
- UI backend: Qt widgets, labels, buttons, progress, redraw
- input backend: Qt mouse/keyboard interactions

Build path:
- configure with CMake
- build `spotify-recorder-qt`

## Notes
- Current Linux build is clean.
- The repo is already moving toward a portable core + adapter model.
