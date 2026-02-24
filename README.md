# mp4player

Minimal Win32 MP4 player using FFmpeg for decode.

## Build (Windows) with CMake

Prereqs:
- CMake
- A C compiler (Visual Studio Build Tools is easiest)
- FFmpeg dev+bin bundle. This repo already includes one at `./ffmpeg/` (must contain `include/`, `lib/`, `bin/`).

### Configure + build

From the repo root:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The post-build step copies FFmpeg DLLs from `ffmpeg/bin` next to the built `.exe`, so it should run immediately.

### If your FFmpeg is elsewhere

```powershell
cmake -S . -B build -DFFMPEG_ROOT="C:\path\to\ffmpeg"
cmake --build build --config Release
```

## Notes

- If the program runs but you get a “missing DLL” error, make sure the FFmpeg `bin/` DLLs are next to the built exe (or in your `PATH`).

