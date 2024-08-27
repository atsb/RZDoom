![rzdoom](https://github.com/user-attachments/assets/32b43db0-9281-4d28-b99a-16a94aa0675e)

The ZDoom you know and love, only better.  ZDoom isn't really dead, despite what the GZ Team say.

RZDoom is fully compatible with all your ZDoom wads, mods, audio etc..  it is only better in every-possible-way.

1. Fully compatible and buildable on Windows, Linux, macOS, BSD..  on x86, x64 and all ARM cpu's (everything that FMOD Studio is compatible with).
2. Fully backwards compatible with ZDoom itself
3. Viciously optimised, in terms of removing bloat and frame-times.  All assembler is removed, all 32bit only code is removed, intrinsics are replaced with modern equivalents.
4. Launcher is improved with more options
5. FMOD Studio is the only audio backend (no more OpenAL) and has been rewritten to be compatible with the absolute latest version of FMOD Studio (as of now in August 2024)
6. Is officially supported by the Brutal Doom Community Expansion team.
7. UMAPINFO support.
8. Remains entirely software renderered, important for some older wads that used HOM effects that won't display correctly on GL/Vulkan ports.

# How to Build

On all platforms except Windows, use CMake and download the FMOD Studio API (you must register).
On Windows, use the provided sln project file.

Alternatively, just wait for releases..
