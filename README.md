# AI Platformer

A momentum-based platformer built with [raylib](https://www.raylib.com/).

## Build

Requires CMake (3.16+) and a C++ compiler (MSVC, MinGW, or Clang). raylib is
fetched and built automatically via CMake's `FetchContent` — no manual install
needed.

```bash
cmake -B build
cmake --build build
```

The executable is placed under `build/` (e.g. `build/Debug/AIPlatformer.exe`
with MSVC, or `build/AIPlatformer.exe` with a single-config generator).

## Project layout

- `src/` — game source code
- `assets/` — textures, sounds, levels (copied next to the executable on build)
