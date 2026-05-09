# Spotify Audio Recorder

This repo now contains:

- the Linux GTK/PulseAudio app
- the Qt Widgets/Windows port in progress
- shared transport/core helpers in `core.c`

For implementation details, start with:

- `BUILD_SPEC.md`
- `IMPLEMENTATION_CHECKLIST.md`
- `WINDOWS_PORT.md`

## Linux Build

```bash
make
```

This builds the GTK app as `spotify-recorder`.

## Windows Qt Build

```bat
.\build-run-qt.bat
```

This configures and builds the Qt app with CMake, then deploys and runs it.

## Current Shape

- Linux remains the proven GTK/PulseAudio path.
- Windows uses Qt Widgets plus a Windows audio backend.
- Common state and transport logic is being moved into shared code.

## Notes

- Linux records system audio via the default sink monitor.
- Windows uses the same peak-buffer waveform model as Linux.
- The goal is to keep platform-specific code thin and share behavior wherever it is truly common.
