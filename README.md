# Spotify Audio Recorder

This repo contains a GTK desktop app skeleton for a fuller audio recorder, plus the original PulseAudio prototype and planning docs.

For the next implementation pass, start with:

- `BUILD_SPEC.md`
- `IMPLEMENTATION_CHECKLIST.md`

## Build

```bash
make
```

This builds the GTK app skeleton as `spotify-recorder`.

## Run

```bash
./spotify-recorder
```

The current app is a UI scaffold with Record, Pause, Stop, Play, and speed controls.

## How it works

1. Connects to PulseAudio.
2. Reads the default sink name.
3. Appends `.monitor` to capture the output of that sink.
4. Records 16-bit stereo PCM at 44.1 kHz.
5. Streams samples until stopped.

## Notes

- This records everything playing on your output device, not just Spotify.
- Stop with `Ctrl-C`.
- The longer-term goal is an app with an in-memory buffer, waveform display, and speed control.
- Loop/transport implementation lessons are documented in `BUILD_SPEC.md` and regression checks are in `IMPLEMENTATION_CHECKLIST.md`.
