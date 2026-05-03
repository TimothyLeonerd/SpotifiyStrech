# Windows Port Notes

## Goal
Port the current Linux GTK3/PulseAudio app to Windows while keeping the existing core behavior.

## Current Architecture
- `core.c` / `core.h`: portable logic
- `app.c`: GTK3 + PulseAudio adapter/UI layer
- `Makefile`: Linux build for GTK3, PulseAudio, Rubber Band

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
- GTK UI -> Windows UI toolkit of choice
- PulseAudio -> Windows audio API/backend
- pointer/input handling -> Windows event system
- drawing -> Windows drawing API or chosen UI toolkit

## Recommended Next Step
Keep `core.c` unchanged and replace only the platform adapters.

Suggested adapter split:
- audio backend: capture/playback/device selection
- UI backend: labels, buttons, progress, redraw
- input backend: seek/scrub/loop interactions

## Notes
- Current Linux build is clean.
- The repo is already moving toward a portable core + adapter model.
