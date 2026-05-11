# Windows Port Notes

## Goal
Port the current app to a Qt Widgets UI on Windows while keeping the proven Linux behavior and shared core logic intact.

## Current Architecture
- `core.c` / `core.h`: shared helper logic for transport decisions, loop math, seek math, and status text
- `recorder_core.c` / `recorder_core.h`: portable Recorder state shape and lifecycle initialization
- `playback_renderer.c` / `playback_renderer.h`: shared Rubber Band S16 playback-buffer renderer
- `app.c`: Linux GTK3 + PulseAudio app path
- `qt_main.cpp` / `qt_recorder_window.cpp`: Qt Widgets UI path
- `windows_main.c` / `platform_windows_win32.c`: old native Win32 host path, retained in-tree but no longer built as a target
- `Makefile`: Linux build for GTK3, PulseAudio, Rubber Band
- `CMakeLists.txt`: Qt/Windows build path

## Canonical Status
- `WINDOWS_PORT_STATUS.md` is the live source of truth.
- `SHARED_RECORDER_REFACTOR_PLAN.md` is the current architecture plan for eliminating Windows-only transport divergence.
- Update `WINDOWS_PORT_STATUS.md` every time Windows behavior changes.

## Portable Core
Already extracted into `core.c` / `core.h`:
- `AppMode`, `LoopState`, `LoopSnapshot`, `AudioBuffer`, `RenderIntent`
- transport decisions and plans
- UI-derived state snapshots
- status text generation
- loop math
- cursor/playhead math
- recording session reset

Not yet extracted:
- the full Linux-like `RecorderCore` controller/state machine
- authoritative ownership of mode/cursor/loop/speed/render lifecycle across GTK and Qt
- Linux's playback-buffer validity guard as shared controller behavior

Partially extracted:
- Linux `Recorder` now embeds `RecorderCore` while preserving existing field access.
- Qt stores mode, loop, speed, cursor, and scrub fields in `RecorderCore` through transitional reference members.
- Qt stores captured-frame/sample-rate snapshots in `RecorderCore.audio`.
- Qt stores render generation/render-pending state in `RecorderCore` through transitional reference members.
- `recorder_core.c` owns shared OS-independent state transitions for captured timing, playhead reset, play/pause state changes, seek state, tick/playhead updates, and render begin/finish bookkeeping.
- `recorder_core.c` owns OS-independent transport decision/application helpers for record, stop, play-from-idle preparation, pause/resume state, and render-intent toggling.
- Linux GTK uses shared `RecorderCore` transport helpers more directly.
- `recorder_core.c` owns shared seek/scrub lifecycle helpers.
- `recorder_core.c` owns shared loop enable, loop region, loop ratio, waveform press resolution, and loop drag/update/clear helpers.
- `recorder_core.c` owns playback-buffer validity checks/invalidation and Linux render lifecycle state transitions.
- Qt/Windows still has platform I/O sequencing and backend transport ownership outside `RecorderCore`.
- Windows backend validity is narrower now: seek does not invalidate prepared playback, prepared playback is reused when requested speed matches prepared speed, and speed changes are validated at start/prepare time.
- Windows backend no longer stores requested speed as persistent transport state; prepare receives speed explicitly from Qt/`RecorderCore`.
- `RecorderCore` has portable raw PCM storage for Windows/non-GLib paths, and Windows capture appends raw PCM there. Backend-private raw PCM remains only as fallback when no `RecorderCore` is attached.
- Windows Qt passes explicit source PCM, source format, captured-frame count, and speed into backend playback preparation, so the backend no longer chooses the source recording for Qt playback.
- Windows backend snapshots report facts only and no longer mutate Qt/`RecorderCore` app mode.
- Windows render progress polling no longer clears `RecorderCore.render_pending`; render completion owns final mode changes.
- Windows backend still owns WASAPI playback buffer staging, render progress state, playback cursor byte estimates, and live playback-thread loop snapshots; these are now treated as platform I/O/runtime state and need Windows validation.

## Linux Behavior Contract
- The Windows port must preserve Linux architectural rules.
- all shared user-visible behavior should match Linux unless Windows-specific behavior is explicitly documented
- playback cursor is a source-frame cursor, not a raw-byte cursor
- seek changes the playback cursor and the active playback path must honor it
- loop edits only change loop boundaries; they do not restart playback or move the cursor by themselves
- playback uses derived playback data when needed; raw capture data stays the source of truth
- UI refresh must not rewrite transport state unless it is syncing from the real backend snapshot
- playback and scrubbing should follow the same stop/resume semantics as `app.c`

If a Windows implementation cannot satisfy these rules, it is not equivalent to Linux and should be treated as a bug.

## Platform-Specific Layer
Still platform-specific:
- GTK widget creation and callbacks on Linux
- Qt widget creation and callbacks on Windows Qt
- old Win32 host plumbing is not the active Windows path
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
The Linux Rubber Band render pipeline is now in a shared renderer module and linked into the Qt build. The Windows backend converts WASAPI mix-format capture data to the renderer's S16 interleaved PCM contract, then converts rendered S16 back to the WASAPI mix format for playback. Qt runs Windows prepare/render work on a background thread and polls real renderer progress from the backend.

The next step is not more Windows-specific transport patching. Follow `SHARED_RECORDER_REFACTOR_PLAN.md`: extract a shared Linux-like Recorder/controller layer and make both GTK and Qt use it for mode, cursor, loop, speed, render, seek, scrub, and playback-buffer validity.

Keep `core.c` useful for small shared helpers, but do not treat it as the final source of truth for the app state machine.

Do not introduce alternate Windows-only transport models if Linux already defines the intended behavior.

Suggested adapter split:
- audio backend: capture/playback/device selection
- UI backend: Qt widgets, labels, buttons, progress, redraw
- input backend: Qt mouse/keyboard interactions

Build path:
- configure with CMake
- build `spotify-recorder-qt`

## Notes
- Current Linux build is clean.
- The repo is already moving toward a portable Recorder/controller + adapter model.
- Avoid adding Windows-only "shared" code unless Linux also uses the same behavior.
