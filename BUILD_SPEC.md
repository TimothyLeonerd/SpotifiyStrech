# Desktop Audio Recorder Build Spec

## Goal

Build a Linux desktop app that can:

1. Record the system audio output from PulseAudio.
2. Pause / resume / stop recording.
3. Keep audio in an in-memory buffer.
4. Show a visual representation of the buffer.
5. Play back the captured audio at adjustable speed.
6. Optionally keep a second processed buffer for speed-changed playback.

The current prototype is `app.c`, which is the GTK app scaffold. Use that as the starting point, and ignore `pulseTests/`.

## Product Shape

This should become a small native desktop app, not a CLI tool.

Suggested controls:

- Record / Pause
- Stop
- Play
- Speed slider
- Buffer timeline / waveform

## Core Audio Plan

### Capture

- Capture from the PulseAudio default sink monitor.
- Store captured PCM as 16-bit little-endian stereo at a fixed sample rate.
- Start with the same format as the prototype: `44100 Hz`, `S16LE`, `2 channels`.

### Buffering

Use a ring buffer or append-only block buffer for raw PCM.

Store data as frames, not bytes, so the UI and playback code can reason in time.

Keep metadata:

- sample rate
- channel count
- frame count
- capture state
- current write position
- current play position

### Processed Buffer

Keep a second buffer for speed-adjusted audio only if needed.

Recommended rule:

- Raw buffer is the source of truth.
- Processed buffer is derived data and can be regenerated when speed changes.

This avoids corrupting the original recording.

## Playback Speed

Two possible interpretations:

1. Speed changes pitch too.
- Simpler.
- Can be implemented by resampling.

2. Speed changes without pitch shift.
- Better UX for most users.
- Requires a time-stretch library such as Rubber Band or SoundTouch.

If unsure, implement pitch-changing speed first, then upgrade later.

## Visualization

Show the buffer as one of these:

- waveform overview
- rolling amplitude meter
- timeline with playhead and capture progress

For the first version, compute downsampled peak amplitudes per window, then draw those as a waveform.

Suggested data for visualization:

- per-window peak
- per-window RMS
- capture duration
- playhead position

## Suggested Architecture

### Modules

1. `audio_capture`
- Owns PulseAudio connection.
- Reads PCM frames.
- Pushes frames into raw buffer.

2. `audio_buffer`
- Ring buffer or block list.
- Exposes read/write by frame index.

3. `audio_process`
- Generates speed-adjusted output buffer.
- Rebuilds when speed changes.

4. `audio_playback`
- Reads from raw or processed buffer.
- Feeds output to PulseAudio or another playback backend.

5. `ui`
- Buttons, slider, waveform, state text.

### Threading

- UI thread: window, controls, waveform drawing.
- Capture thread: records from PulseAudio.
- Processing thread: regenerates speed-adjusted buffer.
- Playback thread: streams audio for play mode.

Use locks or message passing carefully. Keep the UI responsive.

## Common Issues Found During Loop Implementation

- Keep UI state edits pure. Toggling loop should only change `loop_enabled`; dragging a marker should only change the loop boundary value.
- Do not let UI edits directly restart playback, reset the cursor, or invalidate active audio unless the user explicitly seeks/stops/starts.
- Treat playback position as a source-frame cursor. Rendered/time-stretched buffers are derived implementation details and must not become the source of truth for transport state.
- Avoid separate hidden meanings for visible loop markers. If the UI draws full-buffer loop handles, playback and hit testing must treat `0..captured_frames` as the effective loop region too.
- Do not represent the same concept in two places without a clear rule. `loop_region_set == false` plus visible full-width markers caused playback and UI to disagree.
- Playback should consult loop state only at boundary decisions. Loop state can decide whether the next boundary wraps, but should not cause immediate cursor movement while playback is mid-segment.
- Avoid coupling loop edits to segment rendering. Re-rendering a stretched segment during marker drag can re-anchor output offsets and make audio/playhead jump.
- Be careful with lock ownership. Do not call helper functions that lock `Recorder.mutex` while already holding that mutex.
- Keep transport and loop concerns separate. Scrubbing/seeking changes playback cursor; loop editing changes loop boundaries.
- If a full architecture refactor happens, split state into transport state, loop state, playback engine, waveform view, and GTK controller instead of letting all callbacks mutate one shared struct arbitrarily.

## Recommended Stack

- C or C++ on Linux
- GTK4 for the desktop UI
- libpulse for capture and playback integration

If the buffer and UI logic become large, C++ may be easier than plain C.

## Milestones

### Milestone 1

- Convert the existing recorder into an app with a window.
- Add Record, Pause, Stop buttons.
- Capture audio into memory instead of immediately writing to file.

### Milestone 2

- Add waveform visualization.
- Add play from buffer.

### Milestone 3

- Add speed slider.
- Add processed buffer generation.
- Decide whether pitch preservation is required.

### Milestone 4

- Polish UX.
- Add hotkeys.
- Add error handling for missing PulseAudio devices.

## Implementation Notes

- Preserve the raw recording exactly.
- Do not let the speed slider destroy the original buffer.
- Make buffer size configurable.
- If memory usage becomes a problem, spill old audio to disk or use chunked storage.
- The first version can ignore fancy editing features.

## Open Questions

Before implementing, decide:

1. Do you want pitch to change with playback speed?
2. Should recording be system-wide audio or Spotify-only?
3. Should the buffer be infinite or have a fixed max length?

## Suggested Start Point

1. Keep the current PulseAudio capture logic.
2. Add in-memory PCM buffering.
3. Hook capture into the GTK window.
4. Add waveform rendering once capture works in-app.
