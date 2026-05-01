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
