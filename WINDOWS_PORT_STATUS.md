# Windows Port Status

This file is the canonical live status for the Windows port.

Update this file whenever Windows or shared behavior changes.

The next architectural milestone is documented in `SHARED_RECORDER_REFACTOR_PLAN.md`.

## Checklist

### Done
- [x] Linux GTK/PulseAudio remains the full baseline.
- [x] Windows has a working Qt shell.
- [x] Shared core logic exists in `core.c` / `core.h`.
- [x] Windows has a WASAPI-style audio backend.
- [x] Windows transport buttons work at a basic level.
- [x] Windows waveform rendering shows backend peaks.
- [x] Waveform mouse interaction exists in the Qt UI.
- [x] Scrubbing / click-to-seek works in the Qt UI.
- [x] Windows builds and reuses a full prepared playback buffer, matching Linux's raw/playback-buffer split.
- [x] Seek restarts playback from the current cursor.
- [x] Windows seek positions are quantized to PCM frame boundaries.
- [x] Windows reports an estimated audible playback cursor instead of queued WASAPI bytes.
- [x] Some transport decision helpers are shared.
- [x] Loop math helpers are shared.
- [x] Seek/playhead math is shared.
- [x] Status text generation is shared.
- [x] Recording session reset logic is shared.
- [x] Windows Qt is the active Windows UI target; the stale native Win32 executable is no longer built.
- [x] Loop region creation and drag editing exist in the Qt UI.
- [x] Loop-aware playback wraparound is implemented in the Windows playback thread.
- [x] Rubber Band rendering has been extracted into shared `playback_renderer.c` / `playback_renderer.h`.
- [x] The Windows Qt target links the shared renderer and bundled Rubber Band single-file implementation.
- [x] Windows backend converts WASAPI mix-format PCM to S16 for Rubber Band and converts rendered S16 back to the WASAPI mix format.
- [x] Windows speed changes invalidate/rebuild the prepared playback buffer and restart active playback.
- [x] Windows Rubber Band rendering runs off the Qt UI thread.
- [x] Shared renderer reports progress and Windows Qt shows real render progress.
- [x] Windows backend snapshots/progress are no longer blocked by Rubber Band rendering work.
- [x] Portable `RecorderCore` state shape exists in `recorder_core.h` / `recorder_core.c`.
- [x] Linux `Recorder` embeds `RecorderCore` as the first low-risk extraction step.
- [x] Qt stores mode, loop, speed, cursor, and scrub fields in `RecorderCore` through transitional reference members.
- [x] Qt stores captured-frame/sample-rate snapshots in `RecorderCore.audio`.
- [x] Qt stores render generation/render-pending state in `RecorderCore` through transitional reference members.
- [x] Shared `RecorderCore` transition helpers exist for captured timing, playhead reset, play/pause state changes, seek state, tick/playhead updates, and render begin/finish bookkeeping.
- [x] Shared `RecorderCore` transport decision/application helpers exist for record, stop, play-from-idle preparation, pause/resume state, and render-intent toggling.
- [x] Linux GTK uses shared `RecorderCore` transport helpers more directly.
- [x] Shared `RecorderCore` seek/scrub lifecycle helpers exist and are used by GTK/Qt paths.
- [x] Shared `RecorderCore` loop enable/region/ratio/waveform-press/drag helpers exist and are used by GTK/Qt paths.
- [x] Shared `RecorderCore` playback-buffer validity checks/invalidation exist and are used by Linux GTK.
- [x] Shared `RecorderCore` render lifecycle helpers exist for begin, generation validation, progress timing, cancellation, completion intent, and rendered playback-buffer installation.
- [x] Windows backend no longer invalidates prepared playback merely because of seek.
- [x] Windows backend reuses prepared playback when requested speed still matches prepared speed.
- [x] Windows backend speed changes no longer directly clear prepared-buffer readiness; start/prepare validate requested speed against prepared speed.
- [x] Windows backend no longer stores requested speed as persistent transport state; prepare receives speed explicitly from Qt/`RecorderCore`.
- [x] `RecorderCore` has portable raw PCM storage for Windows/non-GLib paths.
- [x] Windows capture appends raw PCM into `RecorderCore`; backend-private raw PCM remains only as fallback when no `RecorderCore` is attached.
- [x] Windows Qt passes explicit source PCM, source format, captured-frame count, and speed into backend playback preparation.
- [x] Windows backend no longer chooses the source recording for Qt playback preparation.
- [x] Windows backend snapshots no longer mutate Qt/`RecorderCore` app mode.
- [x] Windows render progress polling no longer clears `RecorderCore.render_pending`; render completion is the sole render finalizer.
- [x] Windows capture synthesizes silent PCM when WASAPI loopback reports no packets, so no-source recording still advances duration with a flat waveform.
- [x] Windows playback cursor is derived from WASAPI queued padding during playback, so loop wraps report the audible position instead of the write-ahead cursor.
- [x] Windows loop cursor math remains linear before the first actual queued loop wrap, so seeking before a loop does not display a circular-loop position early.
- [x] Windows loop playback uses Linux-style temporary loop arming: seeking past loop end continues linearly instead of forcing an immediate wrap.
- [x] Linux and Windows playback loops now use shared `CorePlaybackTimeline` stepping for source/output cursor conversion, loop arming, loop wrap, and end-of-playback detection.
- [x] Linux and Windows speed changes now use shared `RecorderCoreSpeedChange` policy for invalidation, render restart, idle/paused pre-render, and active-playback restart decisions.
- [x] Linux playback requests now use shared `RecorderCorePlaybackRequest`/ready-start policy, and Windows play-from-idle uses the same request policy before starting async prepare.
- [x] Linux capture append now flows through `recorder_core_append_pcm`, matching Windows raw PCM append invalidation/captured-frame policy.
- [x] Loop markers now persist as always-visible marker positions; moving markers no longer enables loop playback, and loop fill/wrapping is controlled only by the Loop button.
- [x] While recording, loop marker ratios remain visually stable and are materialized to audio frame positions when recording stops.
- [x] Live loop-marker dragging is ratio-based and does not auto-expand close markers; start/end ordering is normalized only when dragging ends.

### Partial
- [ ] Windows seek/scrub parity still needs runtime regression testing on real WASAPI output.
- [ ] Windows loop playback needs runtime validation against Linux edge cases.
- [ ] Windows loop cursor/audio sync needs runtime validation after the WASAPI padding-based cursor change.
- [ ] Windows speed/render parity needs runtime validation, especially cursor mapping during non-1.0x playback.
- [ ] Windows no-source recording needs runtime validation on real WASAPI output.
- [ ] Windows transport/state ownership is partially shared; Qt still owns platform I/O sequencing and the Windows backend still owns transport state that should move into `RecorderCore`.
- [ ] Windows backend still owns WASAPI playback buffer staging, render progress state, playback cursor byte estimates, and live playback-thread loop snapshots; these are now treated as platform I/O/runtime state and need Windows validation.

### Missing
- [ ] Windows render-mode transport using the Linux async render intent model.
- [ ] Shared Linux-like `RecorderCore` model used by both GTK and Qt.
- [ ] Shared ownership of playback-buffer validity, cursor, loop, speed, render lifecycle, seek, and scrub state.

## Maintenance Rule

- If you change Windows behavior, update this file in the same change.
- If this file and the code disagree, trust the code and refresh the file immediately.
- Windows parity means matching Linux behavior across the full feature set, not just matching visible controls.
- Prefer changes that move Windows toward `SHARED_RECORDER_REFACTOR_PLAN.md` over additional Windows-only transport patches.
