# Windows Port Notes

## Goal
Port the current app to a Qt Widgets UI on Windows while keeping the proven Linux behavior and shared core logic intact.

## Current Architecture
- `core.c` / `core.h`: shared transport, loop, seek, and status logic
- `app.c`: Linux GTK3 + PulseAudio app path
- `qt_main.cpp` / `qt_recorder_window.cpp`: Qt Widgets UI path
- `windows_main.c` / `platform_windows_win32.c`: native Win32 host path
- `Makefile`: Linux build for GTK3, PulseAudio, Rubber Band
- `CMakeLists.txt`: Qt/Windows build path

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
Still platform-specific:
- GTK widget creation and callbacks on Linux
- Qt widget creation and callbacks on Windows Qt
- Win32 host plumbing for the native Windows window
- Cairo waveform drawing on Linux
- Qt painting on Windows Qt
- PulseAudio capture/playback on Linux
- Windows audio backend on Windows

## App Behavior To Preserve
- record / stop / play-pause
- click-to-seek and scrubbing
- loop region creation and adjustment
- pitch-preserving speed changes via Rubber Band
- async render of playback buffer
- recording-time seek should survive into playback
- peak-buffer waveform visualization should be shared in spirit, not reimplemented as separate UI logic

## Likely Windows Replacements
- GTK UI -> Qt Widgets
- PulseAudio -> Windows audio API/backend
- pointer/input handling -> Windows event system
- drawing -> Qt painting

## Recommended Next Step
Keep `core.c` as the source of truth for shared state transitions, and move any duplicated Windows/Qt transport behavior to it instead of re-encoding it in UI code.

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
- Avoid adding Windows-only "shared" code unless Linux also uses the same behavior.
