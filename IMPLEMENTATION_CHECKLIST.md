# Implementation Checklist

## Phase 1: App Skeleton

- Create a GTK window.
- Add `Record`, `Pause`, `Stop`, and `Play` controls.
- Add a speed slider with a default value of `1.0x`.
- Add a placeholder area for waveform or timeline rendering.

## Phase 2: Capture and Buffering

- Move PulseAudio capture into a background component.
- Capture from the default sink monitor.
- Store raw PCM frames in memory.
- Track sample rate, channels, frame count, and current state.
- Add pause/resume behavior without losing buffered data.

## Phase 3: Playback

- Add playback from the raw buffer.
- Wire the play/pause/stop controls into playback state.
- Make playback position visible in the UI.

## Phase 4: Visualization

- Generate downsampled amplitude peaks from the raw buffer.
- Draw a basic waveform or timeline.
- Show capture progress and playhead position.

## Phase 5: Speed Control

- Apply the slider to playback speed.
- Decide whether playback should preserve pitch.
- If needed, add a derived processed buffer for speed-adjusted audio.
- Recompute processed data when speed changes.

## Phase 6: Stability

- Handle PulseAudio disconnects and missing devices.
- Keep the UI responsive while recording and processing.
- Set reasonable buffer size limits.
- Test long captures and repeated pause/resume cycles.

## Loop and Transport Regression Checklist

- Loop toggle only changes `loop_enabled`; it does not seek, restart playback, or create hidden regions.
- Dragging loop start/end only changes loop boundary values; it does not move the playhead or restart playback.
- If no explicit loop region exists, the effective loop region is the full captured buffer (`0..captured_frames`).
- Visible loop handles must match the effective loop region used by playback and hit testing.
- Playback cursor is tracked in source frames, not output/rendered-buffer frames.
- Rendered Rubber Band output is treated as disposable derived data.
- Playback only wraps at loop boundaries; loop state changes should not cause immediate jumps mid-segment.
- Seeking/scrubbing is the only timeline interaction that directly changes playback cursor.
- Speed changes may restart/re-render playback, but loop toggle/marker edits should not.
- Test initial full-buffer loop: turn loop on with no custom region, play to buffer end, confirm it wraps to start.
- Test initial marker drag: drag both full-width boundary handles before any custom region exists.
- Test custom loop: create a region, play through the end, confirm it wraps to the region start.
- Test loop off: disable loop during playback and confirm playback continues forward without cursor jumps.
- Test marker drag during playback and confirm the playhead/audio do not jump merely because the marker moved.
