/* gbdefs.h — hardware constants and the core logging interface.
 *
 * This header is included by every core module. It must never include <SDL.h>:
 * the core is a plain C library that the SDL host drives, which is what lets the
 * same core run headless in CI and, later, in a WebAssembly build.
 *
 * Reference implementation for CS 4XX "System Emulation".
 * Read this file top to bottom: it is the vocabulary the whole project speaks.
 */
#ifndef GB_DEFS_H
#define GB_DEFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* --- Clocks and frame geometry -----------------------------------------
 * The SM83 CPU runs at 4.194304 MHz; one "T-cycle" is one CPU clock tick.
 * A video frame is 154 scanlines of 456 dots each, which gives 70224
 * T-cycles per frame, i.e. 59.7275 frames per second. Every timing constant
 * in this project is expressed in T-cycles, never in wall-clock time.
 */
#define CPU_CLOCK_HZ  4194304u
#define FRAME_TICKS   70224u            /* 154 * 456 */
#define SCREEN_W      160
#define SCREEN_H      144

/* --- Audio -------------------------------------------------------------
 * The APU is clocked at the CPU clock divided by 4, i.e. 1.048576 MHz. We
 * generate samples at that rate and let the host resample to the device rate.
 */
#define APU_CLOCK_HZ  1048576u
#define SAMPLE_RATE   48000
#define APU_BUF_FRAMES 4096             /* stereo frames in the ring buffer */

/* --- Memory sizes ------------------------------------------------------ */
#define VRAM_BANK_SIZE 0x2000u           /* 8 KiB per bank, 2 banks on CGB  */
#define VRAM_BANKS     2u
#define WRAM_BANK_SIZE 0x1000u           /* 4 KiB per bank, 8 banks on CGB  */
#define WRAM_BANKS     8u
#define OAM_SIZE       0xA0u              /* 40 sprites * 4 bytes            */
#define HRAM_SIZE      0x7Fu              /* 127 bytes, always CPU-accessible */

/* --- Interrupt bits (shared by IE at FFFF and IF at FF0F) --------------- */
#define IF_VBLANK 0x01u                  /* vector 0x40 */
#define IF_STAT   0x02u                  /* vector 0x48 */
#define IF_TIMER  0x04u                  /* vector 0x50 */
#define IF_SERIAL 0x08u                  /* vector 0x58 */
#define IF_JOYPAD 0x10u                  /* vector 0x60 */

/* --- Logging -----------------------------------------------------------
 * The core must not call SDL_Log (that would drag SDL into the core). It calls
 * gb_log(), and the host installs a sink in gb_init(). The default sink writes
 * to stderr so a headless build still reports problems.
 */
typedef void (*gb_log_fn)(const char *line);
void gb_log_set_sink(gb_log_fn sink);
void gb_log(const char *fmt, ...);

/* --- Save-state helpers -------------------------------------------------
 * Save states use a raw struct dump. This is deliberately simple: it is
 * same-machine, same-build only, which the README states explicitly. A portable
 * format (fixed-width, explicit byte order) is the capstone stretch goal.
 */
#define GB_SER(x, f)   (fwrite(&(x), sizeof(x), 1, (f)) == 1)
#define GB_DESER(x, f) (fread(&(x), sizeof(x), 1, (f)) == 1)

#endif /* GB_DEFS_H */
