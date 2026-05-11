# Spotify Audio Recorder

This repo now contains:

- the Linux GTK/PulseAudio app
- the Qt Widgets/Windows port in progress
- shared transport/core helpers in `core.c`

For implementation details, start with:

- `WINDOWS_PORT_STATUS.md`
- `BUILD_SPEC.md`
- `IMPLEMENTATION_CHECKLIST.md`
- `WINDOWS_PORT.md`
- `SHARED_RECORDER_REFACTOR_PLAN.md`

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
- Windows uses Qt Widgets plus a Windows audio backend; the old native Win32 executable path is not built.
- Common helpers and rendering are shared, but full transport/state ownership is still being moved into a shared Recorder/controller model.

## Notes

- Linux records system audio via the default sink monitor.
- Windows uses the same peak-buffer waveform model as Linux.
- The goal is to keep platform-specific code thin and share behavior wherever it is truly common.
- The next architecture target is documented in `SHARED_RECORDER_REFACTOR_PLAN.md`.
