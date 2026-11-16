# DidjDroid

An Android emulator for the **LeapFrog Didj** (2008) handheld, built as full hardware emulation
of the LF1000 (MagicEyes Pollux) system-on-chip. It boots the real Didj firmware from a NAND dump.

## Status

| Part | State |
|---|---|
| ARM926EJ-S CPU (ARMv5TE, ARM + Thumb, CP15 MMU) | Working. Fuzz-tested against Unicorn's ARM926 |
| NAND boot: real LeapFrog bootloader → Linux 2.6.20 kernel | Working |
| Root filesystem, UBI "Brio" partition, LeapFrog app stack | Working |
| Interrupts, timers, GPIO buttons, UART console, RTC, ADC, SPI, DMA | Working (basic) |
| Display: MLC 2D RGB layers | Working: health warning + LeapFrog copyright screens render |
| Audio | PCM sent to the audio block is captured and played (untested on device) |
| 3D graphics (OpenGL ES) | **Working via HLE.** Calls into LeapFrog's `libopengles_lite.so` are trapped and run by a built-in OpenGL ES 1.1 software renderer (`gles.cpp`); the Didj menus render |
| Cartridges | Not yet. The firmware expects raw cart NAND; FAT32 "Cartridge" images need converting |
| Speed | Interpreter only. Boot is CPU-heavy; menus use ~12% of the emulated CPU thanks to the GL HLE |

## Using it

1. Build the APK (GitHub Actions → *Build* → artifact `DidjDroid-apk`).
2. On first launch tap the screen and choose your Didj NAND dump
   (`didj_29f2g08aac_2cda.bin`, 276,824,064 bytes = 256 MB + spare area). It's copied into app storage.
3. Controls: on-screen D-pad, A/B, L/R, Home, Hint, Pause; gamepads and keyboards work too.
4. **MENU** → *Show serial console* displays the Linux boot log, which is useful for bug reports.

NAND writes (profiles, settings) are saved back to the app's copy when the app is paused.

## Layout

- `core/src/arm926.*`: CPU interpreter + MMU
- `core/src/hle.*`: tracks where each process maps `libopengles_lite.so`, parses its ELF symbol table and traps `gl*`/`egl*` entry points
- `core/src/gles.*`: OpenGL ES 1.1 + EGL implementation (transform, lighting, clipping, textured rasteriser, blending, fog)
- `core/src/didj.*`: LF1000 system: memory map, NAND chip/controller, IRQ controller, timers,
  GPIO, UART, DMA, SPI, ADC, RTC, MLC/DPC display, GA3D stub
- `app/`: Android frontend (Java + JNI)
- `tools/didjcli.cpp`: desktop test runner (`didjcli nand.bin -f frames -o shot.ppm -k frame:mask -s N`)
- `tests/cpu_fuzz.py`: differential CPU fuzzer against Unicorn

## Hardware references

LeapFrog's GPL kernel sources (LF1000 `mach-lf1000`, `drivers/lf1000`) and the eLinux Didj pages.
