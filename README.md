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
| Cartridges | **Working.** FAT cart images (e.g. `NLR` inside a cart zip) are wrapped in UBI on a virtual cart NAND chip (`cart.cpp`); raw cart NAND dumps also load. Didji Racing: Tiki Tropics boots and plays |
| Serial console | UART with TX interrupts, so userspace logging works (it used to block games) |
| Speed | ARM64 JIT (default on 64-bit phones): about 4.6× fewer host instructions than the interpreter on boot. MENU → CPU core switches back to the interpreter for troubleshooting |

## Using it

1. Build the APK (GitHub Actions → *Build* → artifact `DidjDroid-apk`).
2. On first launch tap the screen and choose your Didj NAND dump
   (`didj_29f2g08aac_2cda.bin`, 276,824,064 bytes = 256 MB + spare area). It's copied into app storage.
3. To play a game: **MENU → Insert cartridge** and pick the cart zip / image. The Didj restarts with it inserted;
   it appears under *My Games*. The cartridge stays inserted until you choose *Eject cartridge*.
4. Controls: on-screen D-pad, A/B, L/R, Home, Hint, Pause; gamepads and keyboards work too.
5. **MENU** → *Show serial console* displays the Linux boot log, which is useful for bug reports.

NAND writes (profiles, settings) are saved back to the app's copy when the app is paused.

## Layout

- `core/src/arm926.*`: CPU interpreter + MMU
- `core/src/jit.*`, `a64emit.h`: ARM→ARM64 dynamic recompiler (block chaining, inline TLB, interpreter fallback); checked against the interpreter by `tests/jit_fuzz.cpp`
- `core/src/hle.*`: tracks where each process maps `libopengles_lite.so`, parses its ELF symbol table and traps `gl*`/`egl*` entry points
- `core/src/gles.*`: OpenGL ES 1.1 + EGL implementation (transform, lighting, clipping, textured rasteriser, blending, fog)
- `core/src/didj.*`: LF1000 system: memory map, NAND chip/controller, IRQ controller, timers,
  GPIO, UART, DMA, SPI, ADC, RTC, MLC/DPC display, GA3D stub
- `app/`: Android frontend (Java + JNI)
- `tools/didjcli.cpp`: desktop test runner (`didjcli nand.bin -f frames -o shot.ppm -k frame:mask -s N`)
- `tests/cpu_fuzz.py`: differential CPU fuzzer against Unicorn

## Hardware references

LeapFrog's GPL kernel sources (LF1000 `mach-lf1000`, `drivers/lf1000`) and the eLinux Didj pages.
