# Shared Recorder Refactor Plan

## Problem

The Linux path works because `app.c` has one authoritative `Recorder` state object. Transport, cursor, loop, render intent, playback-buffer validity, and UI refresh all flow through that state.

The Windows Qt path currently only reuses helper functions from `core.c`. It does not share the Linux state machine. Instead, state is split across Qt fields, `windows_audio_backend.c`, and the playback thread. That split causes recurring bugs:

- stop/play can re-render even when speed and source audio did not change
- cursor position can diverge from audible playback
- loop changes can be stale or delayed during active playback
- speed changes can trigger ad-hoc render/restart paths
- Qt refresh can overwrite local mode/cursor state based on incomplete backend snapshots

The fix is to stop patching the Windows-specific transport model and move the real recorder model into shared code.

## Goal

Create a shared recorder/controller layer used by both Linux GTK and Windows Qt.

Platform-specific code should only handle:

- UI widgets and drawing
- audio device capture/playback I/O
- platform event-loop integration

Shared code should own:

- app mode
- raw capture buffer
- prepared playback buffer
- playback-buffer validity
- speed/render lifecycle
- seek/scrub state
- loop state
- source-frame playback cursor
- status/UI state derivation

## Non-Goals

- Do not create a second Windows-only transport model.
- Do not make WASAPI queued bytes the app cursor.
- Do not hide behavior differences behind Qt UI code.
- Do not fake speed by changing pitch or sample rate if Linux uses Rubber Band.

## Target Architecture

### Shared Recorder Core

Add shared modules, likely:

- `recorder_core.h`
- `recorder_core.c`
- `recorder_render.c` or extend `playback_renderer.c`

The shared recorder owns a state struct similar to Linux's current `Recorder`, minus GTK/Pulse-specific fields.

Suggested state groups:

- `AppMode mode`
- `AudioBuffer audio`
- `LoopState loop`
- `RenderIntent render_intent`
- playback cursor fields:
  - `playback_cursor_frames`
  - `playback_anchor_frames`
  - `playback_anchor_us`
  - `display_playhead_frames`
- speed:
  - `speed`
  - `audio.playback_speed`
  - `audio.playback_rendered_to_source_ratio`
- render lifecycle:
  - `render_pending`
  - `render_generation`
  - `render_source_mode`
  - progress fields
- scrub state:
  - `scrubbing`
  - `resume_after_scrub`
- error/status text

### Shared Operations

Move these Linux behaviors into shared code:

- record/stop/play-pause dispatch
- `ensure_playback_buffer`
- `start_playback_with_ready_buffer`
- speed change handling
- seek handling
- begin/update/end scrub
- loop create/drag/finalize/toggle
- render completion handling
- playback final-state handling
- UI snapshot generation

The shared layer should expose functions such as:

- `recorder_core_dispatch_transport(...)`
- `recorder_core_set_speed(...)`
- `recorder_core_seek_fraction(...)`
- `recorder_core_begin_scrub(...)`
- `recorder_core_update_scrub(...)`
- `recorder_core_end_scrub(...)`
- `recorder_core_waveform_press/move/release(...)`
- `recorder_core_snapshot_ui(...)`

### Platform Audio Backend Contract

Backends should not own app transport semantics.

They should provide I/O operations:

- start capture
- stop capture
- start playback from a prepared buffer
- stop playback
- notify or report playback progress

Backends may own platform device handles and audio threads, but the app cursor remains a source-frame cursor owned by shared recorder state.

### Prepared Playback Buffer Contract

Match Linux exactly:

- raw captured PCM is the source of truth
- prepared playback PCM is full derived playback data for the current source+speed
- seek does not slice the prepared buffer
- playback starts from the source-frame cursor translated into the prepared buffer using `playback_rendered_to_source_ratio`
- playback buffer remains valid across pause/stop unless source audio or speed changes

### WASAPI-Specific Cursor Handling

WASAPI queued bytes are not the app cursor.

The Windows playback backend should report enough hardware timing to estimate audible playback, but shared recorder state should own the source-frame cursor.

Acceptable Windows backend responsibilities:

- keep WASAPI buffer fed
- track queued output position internally
- expose elapsed/audible position or completion events
- never reinterpret loop/seek/speed as app-level state

## Migration Phases

### Phase 1: Extract Shared State Shape

Create `RecorderCore` from Linux `Recorder` fields that are not GTK/Pulse-specific.

Linux should compile and behave the same after this phase.

Deliverables:

- shared struct
- Linux `Recorder` embeds or owns `RecorderCore`
- no behavioral changes intended

### Phase 2: Move Transport Operations

Move Linux transport functions into shared recorder code:

- record plan application
- stop plan application
- play/pause/resume application
- cursor preservation rules

Linux GTK callbacks call shared operations.

Windows Qt callbacks call the same operations.

### Phase 3: Move Render Lifecycle

Move Linux render lifecycle into shared code:

- playback-buffer invalidation
- async render generation
- render intent
- progress
- completion install
- rendered-to-source ratio

Platform UI receives progress snapshots. Platform thread scheduling may stay platform-specific if needed, but state transitions should be shared.

### Phase 4: Move Seek/Scrub/Loop Interactions

Move waveform behavior into shared code:

- click-to-seek
- active-playback seek stop/restart behavior
- scrub begin/update/end
- loop create/drag/finalize/toggle

Qt and GTK should only translate mouse coordinates/modifiers into shared calls.

### Phase 5: Simplify Windows Backend

Remove duplicated transport concepts from `windows_audio_backend.c`:

- backend-owned loop state
- backend-owned speed state
- backend-owned prepared-buffer validity
- app-level cursor decisions

Keep only platform I/O and timing.

### Phase 6: Remove Stale Windows Paths

The old native Win32 host is no longer the active Windows path. Either delete it or keep it clearly marked as experimental/unbuilt. Do not maintain two Windows transport implementations.

## Validation Checklist

Run these on both Linux and Windows:

- record, stop, play
- pause, resume without re-render
- stop, play again without re-render when speed/source unchanged
- seek while playing, audio resumes at target
- scrub while playing, resumes on release
- speed change while paused renders once and resumes later from correct cursor
- speed change while playing renders once and restarts from correct source cursor
- loop on with no custom region wraps full buffer
- custom loop wraps at correct boundary
- loop off during playback stops wrapping immediately
- move loop markers during playback does not move cursor immediately
- cursor and audible audio stay aligned through seeks, loops, and speed changes

## Current Status

Windows has several tactical fixes, including shared Rubber Band rendering and WASAPI format conversion, but it still does not share the Linux recorder state machine. Treat further Windows-specific transport patches as temporary unless they move toward this plan.

Initial extraction has started:

- `recorder_core.h` / `recorder_core.c` define the portable `RecorderCore` state shape.
- Linux `Recorder` embeds `RecorderCore` while preserving existing field access for a low-risk first step.
- Linux initialization now goes through `recorder_core_init`.
- Qt now stores its mode, loop, speed, cursor, and scrub fields in `RecorderCore` through transitional reference members.
- Qt now stores captured-frame and sample-rate snapshots in `RecorderCore.audio`.
- Qt now stores Windows render generation/render-pending state in `RecorderCore` through transitional reference members.
- `recorder_core.c` now owns shared OS-independent state transitions for mode setting, captured timing, playhead reset, play/pause state changes, seek state, tick/playhead updates, and render begin/finish bookkeeping.
- `recorder_core.c` now owns OS-independent transport decision/application helpers for record, stop, play-from-idle preparation, pause/resume state, and render-intent toggling. Qt still executes the platform I/O side effects around those decisions.
- Linux GTK now uses the shared `RecorderCore` transport helpers more directly.
- `RecorderCore` now owns shared seek/scrub lifecycle helpers.
- `RecorderCore` now owns shared loop enable, loop region, loop ratio, waveform press resolution, and loop drag/update/clear helpers.
- `RecorderCore` now owns playback-buffer validity checks/invalidation and Linux render lifecycle state transitions for render begin, generation validation, progress timing, cancellation, completion intent, and rendered playback-buffer installation.
- Windows backend playback-buffer validity is narrower: seek no longer invalidates the prepared buffer, speed changes are checked against the prepared-buffer speed, and prepare reuses an existing buffer when the source and requested speed are still valid.
- Windows backend no longer owns requested speed as persistent transport state; Qt/`RecorderCore` passes the requested speed into prepare, and the backend only records the speed its current WASAPI playback buffer was prepared for.
- `RecorderCore` now has portable raw PCM storage for Windows/non-GLib paths.
- Windows capture appends raw PCM into `RecorderCore`; backend-private raw PCM is retained only as a fallback when no `RecorderCore` is attached.
- Windows Qt now passes explicit source PCM, source format, captured-frame count, and speed into the backend when preparing playback. The backend no longer chooses the source recording for the Qt path.
- Windows backend snapshots now report facts only; they do not mutate Qt/`RecorderCore` app mode.
- Windows render progress polling no longer clears `RecorderCore.render_pending`; render completion owns final mode changes.
- Qt/Windows still has platform-specific render execution and backend transport ownership outside `RecorderCore`.
