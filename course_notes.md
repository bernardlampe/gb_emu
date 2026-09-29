# CS 410 — System Emulation: Building a Game Boy Emulator in C

One semester, ~30 lessons, one growing codebase. Every lesson adds a capability that can be
**demonstrated and tested** before the next lesson starts. Code samples in this document are the
reference implementations shown in lecture: they are heavily commented on purpose, because students
read them as the primary spec.

**The complete reference implementation of this course is this repository.** It is what the teacher
compares student output against, and what students are given at the end of the term: `src/` for the
emulator, `include/` for the interfaces each lesson pins down, `tests/` for the harness that grades
both, and `tools/make_test_roms.py` for the course's own test ROMs (no commercial ROM is used or
shipped anywhere). `README.md` maps each lesson to its file and records what was verified and how.

---

## 1. Course description

Students implement a Game Boy (DMG, with a CGB extension in the final unit) from scratch in C11:
a Sharp SM83 CPU core, a memory bus, cartridge mappers, a scanline PPU with SDL presentation,
a four-channel APU feeding SDL audio, and a debugger. The Game Boy is chosen because it is
*complete* (every subsystem is documented and small enough to finish in a semester), *observable*
(test ROMs produce machine-checkable pass/fail output), and *forgiving* (no OS, no caches, no
privilege levels). The course is really a course in **faithful machine modeling**: reading a hardware
reference, deciding what fidelity is required, and proving the model with evidence.

**Guiding rule used all semester:** *no feature is "done" until a test ROM, a trace diff, or an
observable on-screen behavior proves it.* Students learn early that "it looks right" is not evidence.

---

## 2. Prerequisites

| Prerequisite | Why it is needed |
|---|---|
| C (pointers, structs, `union`, bit manipulation, function pointers) | the entire codebase |
| Assembly reading ability (any ISA) | reading SM83 disassembly in the debugger |
| Basic computer architecture (fetch/decode/execute, memory-mapped I/O, interrupts) | CPU and bus design |
| Basic digital logic (counters, flip-flops, edge-triggered timing) | timer, PPU, APU frame sequencer |
| Git | per-lesson milestone grading |
| **Not** required | prior emulator, prior SDL, prior Game Boy knowledge |

**Tools:** `clang`/`gcc`, CMake ≥ 3.24, `git`, `gdb`/`lldb`, AddressSanitizer + UBSan,
SDL3 (`brew install sdl3` / vcpkg `sdl3`), optional `valgrind`, optional `clang-tidy`.

**Hardware:** any laptop. A physical Game Boy is optional but useful for A/B comparison of PPU and
APU output. Audio work needs headphones, not laptop speakers.

**Legal:** students must not download commercial ROMs or Nintendo's boot ROM. The course uses homebrew
ROMs, the public test suites (Blargg, Mooneye, dmg-acid2, Mealybug Tearoom), and a permissively licensed
boot ROM. This is taught explicitly in L1 and enforced by the submission policy.

---

## 3. Learning outcomes

By the end of the course a student can:

1. Read a hardware reference (Pan Docs) and turn it into correct C.
2. Explain the difference between **functional**, **cycle-approximate**, and **cycle-accurate** modeling,
   and choose the cheapest one that passes the required tests.
3. Build a deterministic, replayable emulator with save states.
4. Instrument and debug an unfamiliar program using a disassembler, breakpoints, and hardware-state viewers.
5. Separate **core** (no I/O) from **host** (SDL) so the core is testable headless.

---

## 4. Reference platform

| Item | Choice | Notes |
|---|---|---|
| Target | DMG (LR35902 @ 4.194304 MHz), CGB as an extension unit | CGB is an added unit, not a requirement to pass |
| Language | C11 (`-std=c11`), no extensions | `-Wall -Wextra -Werror -Wconversion -Wshadow` |
| Windowing / input / audio | SDL3 | SDL2 equivalents are given in an appendix table |
| Build | CMake + `FetchContent` | one command builds on macOS, Linux, Windows |
| Framebuffer | 160×144 `uint32_t` XRGB8888 | PPU writes it; SDL blits it |
| Tests | Blargg `cpu_instrs`, `instr_timing`, `mem_timing`, `dmg_sound`; Mooneye `acceptance`, `emulator-only`; dmg-acid2; Mealybug Tearoom | automated in CI, headless |

### 4.1 SDL3 → SDL2 translation table (given to students once, in L1)

| Purpose | SDL3 | SDL2 |
|---|---|---|
| Create renderer | `SDL_CreateRenderer(win, NULL)` | `SDL_CreateRenderer(win, -1, 0)` |
| Blit texture | `SDL_RenderTexture(ren, tex, NULL, NULL)` | `SDL_RenderCopy(ren, tex, NULL, NULL)` |
| Scale mode | `SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST)` | `SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0")` |
| Audio device | `SDL_OpenAudioDeviceStream(...)` + `SDL_PutAudioStreamData` | `SDL_OpenAudioDevice` + `SDL_QueueAudio` |
| Gamepads | `SDL_OpenGamepad` | `SDL_GameControllerOpen` |
| Ticks | `SDL_GetTicks()` (ms, `Uint64`) | `SDL_GetTicks()` (ms, `Uint32`) |

The course targets SDL3. The table exists so students on distros that still ship SDL2 are not blocked.

---

## 5. Repository layout (grows one file per lesson)

```
gbemu/
  CMakeLists.txt          # L1; gains a target per lesson
  src/
    main.c                # L1  host shell, fixed-timestep loop
    cpu.h  cpu.c          # L2, L5-L10
    mmu.h  mmu.c          # L3-L4
    cart.h cart.c         # L12-L14
    timer.h timer.c       # L9
    ppu.h  ppu.c          # L19-L22
    joypad.h joypad.c     # L23
    apu.h  apu.c           # L24-L27
    debug.h debug.c       # L15-L18
    save.h  save.c           # L14, L29
  tests/                  # test ROMs + headless runner
  roms/                   # gitignored; homebrew/test ROMs only
```

### Build order (mermaid; the graph is the syllabus)

```mermaid
graph LR
  A[L1 SDL shell] --> B[L2 cpu.h registers]
  B --> C[L3 mmu bus]
  C --> D[L4 I/O register map]
  D --> E[L5 fetch/decode/execute]
  E --> F[L6 8-bit ALU + flags]
  F --> G[L7 16-bit ALU + control flow]
  G --> H[L8 CB prefix + DAA]
  H --> I[L9 timers]
  I --> J[L10 interrupts + HALT]
  J --> K[L11 serial + test harness]
  K --> L[L12 cart header]
  L --> M[L13 MBC1/2/5 + battery]
  M --> N[L14 MBC3 + RTC]
  N --> O[L15 trace + disassembler]
  O --> P[L16 breakpoints + stepping]
  P --> Q[L17 VRAM/OAM/tile viewers]
  Q --> R[L18 frame diffing + CI]
  R --> S[L19 PPU registers + modes]
  S --> T[L20 BG + window]
  T --> U[L21 sprites + OAM DMA]
  U --> V[L22 SDL presentation]
  V --> W[L23 joypad + SDL input]
  W --> X[L24 APU + frame sequencer + SDL audio]
  X --> Y[L25 square 1 + sweep]
  Y --> Z[L26 wave + noise]
  Z --> AA[L27 mixing + filters]
  AA --> AB[L28 accuracy ladder]
  AB --> AC[L29 save states + replay]
  AC --> AD[L30 CGB]
  AD --> AE[L31 serial + link + RTC]
  AE --> AF[L32 capstone]
```

---

## 6. Assessment

| Weight | Item | Evidence required |
|---|---|---|
| 30% | 30 lesson milestones (one commit + one demo each) | the lesson's "Done when" check |
| 15% | Debugger milestone (§4) | disassemble, break, inspect, screenshot |
| 15% | Playable milestone (§5) | Tetris/Dr. Mario/Homebrew title playable at 60 fps |
| 10% | Audio milestone (§6) | channel isolation demo + no dropouts for 60 s |
| 20% | Capstone (§8) | accuracy report + one CGB game + replay |
| 10% | Code reviews / quizzes | review another student's bus implementation |

**Definition of done (applies to every lesson):** the lesson's new capability is exercised by the
lesson's verification method, and `cmake --build` is clean under `-Wall -Wextra -Werror` plus
`-fsanitize=address,undefined`. "It compiles" is not done.

**Academic integrity:** never commit Nintendo's boot ROM or any commercial ROM. Never copy a peer's core.
Reading published emulator source (SameBoy, binjgb, Gearboy, Emulator101) is allowed and encouraged *after*
you have written your own version of that subsystem; cite it in the commit message.

---

## 7. Schedule at a glance

★ = difficulty. "Added" marks material outside the five sections the brief named; it is inserted where the
dependency graph forces it, never appended as an afterthought.

| # | Week | Section | Lesson | New capability | ★ |
|---|---|---|---|---|---|
| 1 | 1 | 0 (added) | Toolchain, CMake, SDL3 window, fixed-timestep loop | a black 160×144 rectangle at 59.7275 Hz | ★ |
| 2 | 1 | 1 | SM83 register file and flags | register/flag data model + unit tests | ★ |
| 3 | 2 | 1 | The 64 KiB memory map and the bus | `mmu_read8/write8` with correct routing | ★ |
| 4 | 2 | 1 | I/O register map, boot state, open bus | power-on register values match Pan Docs | ★ |
| 5 | 3 | 2 (added) | Fetch/decode/execute loop, opcode table | executes real instructions from ROM | ★★ |
| 6 | 3 | 2 (added) | 8-bit ALU and flags in anger | `cpu_instrs` progresses past ALU tests | ★★ |
| 7 | 4 | 2 (added) | 16-bit ALU, stack, jumps/calls/returns | `cpu_instrs` reaches control-flow tests | ★★ |
| 8 | 4 | 2 (added) | CB prefix: rotates, shifts, BIT/RES/SET, DAA | `cpu_instrs` reaches CB tests | ★★★ |
| 9 | 5 | 2 (added) | DIV/TIMA/TMA/TAC | timer tests + DIV-APU hook | ★★ |
| 10 | 5 | 2 (added) | IF/IE/IME, interrupt dispatch, HALT/HALT bug | **`cpu_instrs` prints "Passed"** | ★★★ |
| 11 | 6 | 2 (added) | Serial port, headless test harness | automated CI runs Blargg suites | ★★ |
| 12 | 6 | 3 | Cartridge header parse + validation | rejects bad checksums with a diagnostic | ★★ |
| 13 | 6 | 3 | MBC1/MBC2/MBC5 banking + battery RAM | Mooneye `emulator-only` MBC tests pass | ★★★ |
| 14 | 7 | 3 | MBC3 + RTC + persistence | Pokémon-style saves survive restart | ★★★ |
| 15 | 7 | 4 | Tracing and the disassembler | `--trace` output matches known-good disassembly | ★★ |
| 16 | 7 | 4 | Breakpoints, watchpoints, stepping, stack traces | break at `0x40` on VBlank interrupt | ★★ |
| 17 | 8 | 4 | Memory/VRAM/OAM/tile/palette viewers | tile viewer shows correct font tiles | ★★★ |
| 18 | 8 | 4 | Headless frame diffing and test automation | CI diffs 10 frames of a homebrew ROM | ★★★ |
| 19 | 9 | 5 | PPU registers, LCDC/STAT modes, LY/LYC | STAT mode transitions observable | ★★★ |
| 20 | 9 | 5 | BG and window rendering, scrolling, palettes | BG map renders; window opens/closes correctly | ★★★ |
| 21 | 10 | 5 | Sprites, priority, 8×16 mode, OAM DMA | dmg-acid2 passes | ★★★★ |
| 22 | 10 | 5 | SDL window, streaming texture, scaling, pacing | 4× nearest-neighbour image, stable 60 fps | ★★ |
| 23 | 10 | 5 | Joypad matrix + SDL keyboard/gamepad | playable input with correct active-low semantics | ★★ |
| 24 | 11 | 6 | APU architecture, frame sequencer, SDL audio stream | silence → tone through SDL, no underruns | ★★★ |
| 25 | 11 | 6 | Square 1/2: duty, envelope, sweep | channel isolation demo matches `dmg_sound` 01–04 | ★★★★ |
| 26 | 12 | 6 | Wave + noise channels, length counters | `dmg_sound` 05–09 progress | ★★★★ |
| 27 | 12 | 6 | Mixing, NR50/NR51, high-pass/low-pass, resampling | `dmg_sound` 10–12 pass; no DC thump | ★★★★ |
| 28 | 13 | 7 (added) | Accuracy ladder: PPU timing, DMA conflicts, HALT bug | Mooneye `acceptance` pass | ★★★★★ |
| 29 | 13 | 7 (added) | Save states, rewind, deterministic replay | rewind 60 s, bit-identical replay | ★★★★ |
| 30 | 14 | 7 (added) | CGB: double speed, banking, colour palettes, HDMA | CGB game runs in colour; acid2-CGB passes | ★★★★★ |
| 31 | 14 | 7 (added) | Serial/link cable, MBC3 RTC, accessories | two instances exchange a byte | ★★★★ |
| 32 | 15–16 | 8 | Capstone: accuracy report, polish, packaging | release binary + written report | ★★★ |

> If the term is 14 weeks, cut L31 (serial/RTC) and move the capstone into week 14; L28–L30
> carry the remaining "hard" material and should not be cut.

---

## Section 0 — Primer (added; Week 1)

### L1 — Toolchain, build system, and the SDL3 shell
**Difficulty** ★ · **Depends on** nothing · **New capability** a window that presents a 160×144
image at exactly 59.7275 Hz, built by one command.

**Lecture**
- Compilation model: separate translation units, headers vs. sources, why the emulator core must
  not include `<SDL.h>`.
- CMake with `FetchContent` for SDL3; Debug/Release/`RelWithDebInfo`; enabling
  `-fsanitize=address,undefined` in the Debug preset.
- The emulator's **time base**: 4.194304 MHz T-cycles; one frame = 154 lines × 456 dots = **70224
  T-cycles**; 4194304 / 70224 = **59.7275 Hz**.
- Why we run exactly one emulated frame per presented frame (determinism) instead of a wall-clock
  accumulator.
- SDL3 init, window, renderer, streaming texture, nearest-neighbour scaling, vsync.

**Reference code** `src/main.c`

```c
/* main.c — SDL3 host shell.
 *
 * This file is the ONLY place in the project allowed to include <SDL3/SDL.h>.
 * The emulator core (cpu.c, mmu.c, ppu.c, apu.c) must stay host-agnostic so it
 * can be unit-tested headless and reused for the WASM port in the capstone.
 *
 * Reference: DMG frame = 154 scanlines * 456 dots = 70224 T-cycles.
 *            4194304 T-cycles/second / 70224 = 59.7275 frames/second.
 */
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define SCREEN_W 160u          /* DMG visible resolution: 160x144 pixels   */
#define SCREEN_H 144u
#define SCALE    4u            /* 640x576 window: 4x integer scale        */
#define TICKS_PER_FRAME 70224u  /* emulated T-cycles in one video frame     */

/* The PPU owns this buffer; the host only reads it. XRGB8888 is chosen because
 * it is the fastest format for SDL to blit on every desktop GPU backend. */
extern uint32_t g_framebuffer[SCREEN_H][SCREEN_W];

/* One emulated frame: advance every subsystem by TICKS_PER_FRAME T-cycles.
 * Keeping this as a single call (rather than one call per component) is what
 * makes the core deterministic and, later, replayable. */
static void gb_run_frame(void)
{
    /* L5 onward fills this in: CPU/timer/PPU/APU interleaved inside. */
}

int main(void)
{
    /* SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD (L24 adds audio). */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return EXIT_FAILURE;
    }

    SDL_Window *win = SDL_CreateWindow("gbemu", SCREEN_W * SCALE, SCREEN_H * SCALE, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);   /* SDL2: -1 */
    /* STREAMING: the pixels change every frame, so we re-upload instead of
     * allocating a new texture (which would be the "avoidable allocation"
     * mistake this course penalises). */
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         SCREEN_W, SCREEN_H);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);  /* keep 8x8 tiles crisp */

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
        }

        gb_run_frame();  /* emulation: always exactly one frame, no time-based skipping */

        /* Pitch = bytes per row. Passing NULL lets SDL use the natural pitch of
         * the buffer we own; if we ever use a sub-rect we must pass the pitch. */
        SDL_UpdateTexture(tex, NULL, g_framebuffer, SCREEN_W * sizeof(uint32_t));
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);          /* SDL2: SDL_RenderCopy */
        SDL_RenderPresent(ren);                            /* vsync blocks here at 60 Hz */
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return EXIT_SUCCESS;
}
```

**Lab** Build the shell with CMake, display a moving colour-bar pattern, confirm the window refreshes at
59.7 fps, and confirm a Debug build with ASan/UBSan reports zero findings.

**Done when** `cmake --build build && ./gbemu` shows a stable pattern; `grep -c SDL_ src/cpu.c` returns 0
(proves the core/host split from day one).

**Pitfalls** `SDL_RenderClear` without a preceding texture upload; forgetting `SDL_RenderPresent`;
using `SDL_GetTicks()` deltas for frame pacing (non-deterministic); `SCALE` applied by resizing the texture
instead of the destination rectangle (blurry output).

---

## Section 1 — Register maps and memory maps (Weeks 1–2)

> This section is pure data modeling. No emulation logic yet; every lesson is checkable with unit tests.

### L2 — The SM83 register file and flags
**Difficulty** ★ · **Depends on** L1 · **New capability** a correct register/flag data model.

**Lecture**
- The SM83 (a Sharp LR35902, *not* a Z80 and *not* an 8080): registers `A F B C D E H L`, `SP`, `PC`.
- Flag semantics: `Z` (zero), `N` (subtract), `H` (half-carry, bit 3 → bit 4), `C` (carry, bit 7 → bit 8).
  `N` is *sticky for the next DAA* and is why DAA needs it at all.
- 16-bit pairing: `AF`, `BC`, `DE`, `HL`; `HL` is also the memory-indirect pointer.
- Flag register low nibble is hard-wired to zero on hardware; writing `F` must mask with `0xF0`.
- Little-endian byte order in memory; the union trick below and its portability caveat.

**Reference code** `src/cpu.h`

```c
/* cpu.h — the SM83 register file.
 *
 * This header is pure data plus inline helpers: no emulation logic, no I/O, no SDL.
 * Students should be able to include it from a unit test and instantiate a CPU on
 * the stack without pulling in a single dependency.
 */
#ifndef GB_CPU_H
#define GB_CPU_H

#include <stdbool.h>
#include <stdint.h>

/* Flag bits inside F. Only the top four bits exist in silicon; the low nibble
 * reads back as zero, so every write to F must mask with FLAG_MASK. */
enum {
    FLAG_Z = 1u << 7,  /* Zero:     result was 0x00                       */
    FLAG_N = 1u << 6,  /* Subtract: last op was a subtraction (DAA input)   */
    FLAG_H = 1u << 5,  /* Half-carry: carry out of bit 3 (or bit 11)       */
    FLAG_C = 1u << 4,  /* Carry: carry out of bit 7 (or bit 15)            */
};
#define FLAG_MASK 0xF0u

typedef struct {
    /* Anonymous structs inside unions give two names for the same storage, so
     * `cpu->af` and `cpu->a` / `cpu->f` can never disagree.
     *
     * PORTABILITY: this layout assumes a little-endian host, which is true for
     * x86-64, ARM64 and RISC-V. On a big-endian host, F would land in the
     * high byte. Appendix E shows the accessor-function alternative.
     */
    union { struct { uint8_t f, a; }; uint16_t af; };
    union { struct { uint8_t c, b; }; uint16_t bc; };
    union { struct { uint8_t e, d; }; uint16_t de; };
    union { struct { uint8_t l, h; }; uint16_t hl; };

    uint16_t sp;  /* stack pointer: points at the last byte pushed */
    uint16_t pc;  /* program counter: address of the next opcode  */

    bool ime;         /* interrupt master enable (EI sets it, DI clears it) */
    bool ime_pending; /* EI takes effect only AFTER the next instruction  */
    bool halted;      /* HALT executed; resume on any IF & IE match      */
    bool halt_bug;    /* the CPU consumed the HALT byte: re-read it      */

    uint64_t t_cycles; /* total T-cycles since reset; the master clock    */
} CPU;

static inline void cpu_set_flag(CPU *c, uint8_t flag, bool on)
{
    /* Clear first, then set: a single expression per flag keeps the four flags
     * independent and makes the intent readable in the ALU code later. */
    c->f = on ? (uint8_t)(c->f | flag) : (uint8_t)(c->f & ~flag);
}

static inline bool cpu_flag(const CPU *c, uint8_t flag)
{
    return (c->f & flag) != 0u;
}

/* Post-boot-ROM DMG state (Appendix B has the per-model table). Students
 * implementing the boot ROM itself will instead observe these values on exit. */
static inline void cpu_reset(CPU *c)
{
    c->af = 0x01B0;  /* A=0x01, F=0xB0 (Z=1,N=0,H=1,C=1) */
    c->bc = 0x0013;
    c->de = 0x00D8;
    c->hl = 0x014D;
    c->sp = 0xFFFE;
    c->pc = 0x0100;  /* cartridge entry point: ROM0[0x100] */
    c->ime = false;
    c->ime_pending = false;
    c->halted = false;
    c->halt_bug = false;
    c->t_cycles = 0;
}

#endif /* GB_CPU_H */
```

**Lab** Write unit tests for flag masking (`f = 0xFF` must read `0xF0`) and pair/byte aliasing in both
directions (`hl = 0x1234` → `h == 0x12`, `l == 0x34`).

**Done when** the tests pass and the struct is `sizeof(CPU)` printed for the record (students will see it
grow across the term).

**Pitfalls** treating `H` as "carry out of bit 4" (it is *out of bit 3*); forgetting that `N` persists
across instructions until the next ALU operation; using `int` for `t_cycles` (frame counts exceed 32 bits).

---

### L3 — The 64 KiB memory map and the bus
**Difficulty** ★ · **Depends on** L2 · **New capability** every read/write in the machine goes through one
audited function.

**Lecture**
- Address decoding: why the hardware does not use a flat array, and why we must not either.
- Region table: `ROM0`, `ROMX`, `VRAM`, `SRAM`, `WRAM`, `ECHO`, `OAM`, `UNUSED`, `IO`, `HRAM`, `IE`.
- Echo RAM `E000–FDFF` mirrors `C000–DDFF` (subtract `0x2000`); the unused range `FEA0–FEFF` reads
  as open bus (return the last value on the bus), which real games and test ROMs do observe.
- Boot state: which registers and RAM are *unmapped* before the boot ROM hands over.
- The future: this function is where timer/PPU/APU/DMA callbacks will be dispatched (L9, L19–L27).

**Reference code** `src/mmu.c`

```c
/* mmu.c — address decoding.
 *
 * Every memory access in the emulator funnels through these two functions. That is
 * deliberate: it gives one place to add wait states (L28), one place to log bus
 * traffic (L15), and one place for the debugger's watchpoints (L16).
 */
#include "mmu.h"

/* Map an address to the device that owns it. Using explicit comparisons rather
 * than a 16-entry table avoids the "0xFE00-0xFEFF is two regions" trap: OAM is
 * 160 bytes and the 96 bytes after it are unused, so a 4 KiB page table would be
 * wrong at this boundary.
 */
static Region region_of(uint16_t addr)
{
    if (addr < 0x8000) return (addr < 0x4000) ? REGION_ROM0 : REGION_ROMX;
    if (addr < 0xA000) return REGION_VRAM;   /* 8 KiB, 2 banks on CGB   */
    if (addr < 0xC000) return REGION_SRAM;   /* cart RAM, MBC-controlled */
    if (addr < 0xE000) return REGION_WRAM;   /* 8 KiB, banks on CGB      */
    if (addr < 0xFE00) return REGION_ECHO;  /* mirrors C000-DFFF         */
    if (addr < 0xFEA0) return REGION_OAM;   /* 160 bytes of sprite data  */
    if (addr < 0xFF00) return REGION_UNUSED; /* reads as open bus        */
    if (addr < 0xFF80) return REGION_IO;     /* hardware registers       */
    if (addr < 0xFFFF) return REGION_HRAM;   /* 127 bytes, always alive  */
    return REGION_IE;                         /* interrupt enable         */
}

/* Which 4 KiB bank of WRAM owns this address? 0xC000-0xCFFF is always
 * bank 0; 0xD000-0xDFFF is the SVBK bank on the CGB, and bank 1 on the DMG,
 * which has no bank register. The 8 KiB window is banks 0-1 on the DMG and
 * banks 0 and SVBK on the CGB, so the index never runs past wram[]. */
static unsigned wram_bank_of(const MMU *m, uint16_t addr)
{
    if (addr < 0xD000) return 0;
    return m->cgb ? m->wram_bank : 1;
}

uint8_t mmu_read8(MMU *m, uint16_t addr)
{
    switch (region_of(addr)) {
    case REGION_ROM0:
    case REGION_ROMX:
        /* The cart module decides which ROM bank is visible here (L13). */
        return cart_read_rom(m->cart, addr);

    case REGION_VRAM:
        /* PPU mode 3 owns VRAM. On DMG the CPU reads 0xFF while the PPU
         * draws; we return the byte but flag the conflict for L28. */
        return m->vram[m->vram_bank][addr - 0x8000];

    case REGION_SRAM:
        return cart_read_ram(m->cart, addr);

    case REGION_WRAM:
        /* 0xC000-0xCFFF is bank 0; 0xD000-0xDFFF is the SVBK bank
         * (bank 1 on the DMG, which has no bank register). Indexing the
         * whole window with SVBK would run past wram[] for SVBK=7. */
        return m->wram[wram_bank_of(m, addr)][addr & 0x0FFF];

    case REGION_ECHO:
        /* Echo RAM is not storage: it is a mirror. Redirect instead of
         * duplicating, or saves and checksums will silently diverge. */
        return mmu_read8(m, (uint16_t)(addr - 0x2000));

    case REGION_OAM:
        return m->oam[addr - 0xFE00];

    case REGION_UNUSED:
        return m->open_bus;  /* 0xFF in practice; keeps test ROMs happy */

    case REGION_IO:
        return io_read(m, (uint8_t)(addr & 0xFF));

    case REGION_HRAM:
        return m->hram[addr - 0xFF80];

    case REGION_IE:
        return m->ie;
    }
    return 0xFF; /* unreachable, but silences -Wreturn-type */
}
```

**Lab** Implement `mmu_write8` and prove the two rules that students always get wrong: writes to
`ROM0/ROMX` are ignored (they are ROM) and writes to `ECHO` land in `WRAM`.

**Done when** a table-driven test asserts `read(write(addr, v)) == v` for every writable region and
`write(0x2000, v)` changes nothing observable.

**Pitfalls** treating `E000–FDFF` as real storage; forgetting `FEA0–FEFF`; making `0xFF00–0xFF7F` a plain
array (registers have side effects, L17+).

---

### L4 — The I/O register map, power-on state, and open bus
**Difficulty** ★ · **Depends on** L3 · **New capability** a register file whose values match Pan Docs on reset.

**Lecture**
- Full I/O map with owners: `FF00` JOYP, `FF01/02` SB/SC, `FF04–07` DIV/TIMA/TMA/TAC, `FF0F` IF,
  `FF10–FF3F` APU, `FF40–4B` PPU, `FF4D` KEY1, `FF4F` VBK, `FF50` BOOT, `FF51–55` HDMA,
  `FF68–6B` CGB palettes, `FF70` SVBK, `FFFF` IE.
- Read-only vs. read/write vs. write-only registers; masked bits (`IF` top 5, `IE` top 5, `TAC` top 3).
- Reset values (Appendix B) and why `FF44`/`FF41` are dynamic, not stored.
- Open bus: what `mmu->open_bus` is, when it updates, and which tests depend on it.

**Lab** Generate a table of every I/O address with columns *owner*, *R/W*, *mask*, *reset value*; then make
the code assert the mask on every write. This table is reused verbatim by the debugger's I/O viewer in L17.

**Done when** a startup dump of all I/O registers matches Appendix B, and masked writes round-trip.

**Pitfalls** storing `DIV` as a plain byte (it is the high byte of a 16-bit counter); allowing writes to
`FF44` LY; forgetting that unmapped I/O reads return the open bus, not 0xFF.

---

## Section 2 — The CPU core (added; Weeks 3–6; required before anything can run)

> Inserted here because Section 1 produces a data model but nothing executable, and Section 3 (game
> loading) cannot be verified without a CPU that runs the cartridge entry point.

### L5 — Fetch, decode, execute: the opcode table
**Difficulty** ★★ · **Depends on** L4 · **New capability** real instructions execute.

**Lecture**
- The two-level decode: 256 base opcodes, 256 CB-prefixed opcodes.
- Operand kinds: `r8` (A,B,C,D,E,H,L,(HL)), `r16` (BC,DE,HL,SP/AF), `imm8`, `imm16`, `cond` (NZ,Z,NC,C).
- Designing the table as `Instr OPS[256]` with a function pointer plus mnemonic, base cycles, and
  cycles-if-taken. Table-driven beats a 256-arm `switch` for readability and for the disassembler in L15,
  which reuses the same table.
- Illegal opcodes: what hardware actually does and why we `gb_log` them instead of crashing.

**Reference code** `src/cpu.c`

```c
/* One entry per opcode. The disassembler (L15) prints `mnemonic`; the core
 * calls `exec`; the PPU/APU never see this struct. Keeping cycles in the table
 * means timing is data, not code, which is what makes L28 fixable by editing a
 * column instead of rewriting a switch. */
typedef struct {
    const char *mnemonic;                          /* e.g. "LD A, d8"        */
    uint8_t     cycles;                            /* base T-cycles           */
    uint8_t     cycles_taken;                      /* T-cycles if cond taken  */
    void      (*exec)(CPU *c, MMU *m);             /* NULL = illegal opcode    */
} Instr;

/* cpu_step — execute exactly one instruction, return its T-cycles.
 *
 * Callers (the host loop and the timer) treat the return value as the elapsed
 * emulated time. Never measure wall-clock time in here: that is the bug that
 * makes an emulator non-deterministic and un-replayable.
 */
uint32_t cpu_step(CPU *c, MMU *m)
{
    /* Interrupts are serviced BETWEEN instructions, never mid-instruction
     * (L10). Doing it here, not inside the opcode handlers, is what keeps
     * the cycle accounting honest. */
    if (cpu_service_interrupt(c, m))
        return 20;   /* 5 M-cycles = 20 T-cycles to push PC and jump */

    uint8_t op = mmu_read8(m, c->pc++);   /* fetch; PC advances immediately */
    const Instr *in = &OPS[op];
    if (!in->exec) {
        /* Illegal opcode: real hardware locks up. Logging + halting the
         * machine is a debugging aid, not a hardware model. */
        gb_log("illegal opcode %02X at %04X", op, (uint16_t)(c->pc - 1));
        c->halted = true;
        return in->cycles;
    }
    in->exec(c, m);
    return in->cycles;
}
```

**Lab** Fill in the base table for `LD r8,r8'` (`0x40–0x7F`), `LD r8,d8` (`0x06`, `0x0E`, …), and `NOP`.
Every unimplemented entry must be `NULL` so `cpu_step` logs it rather than executing garbage.

**Done when** a hand-written byte array of `0x00 0x3E 0x42 0x00` leaves `A == 0x42` and reports 8+8+4 = 20
T-cycles.

**Pitfalls** off-by-one on `PC` after fetching operands; forgetting that `(HL)` is a memory access that
takes 4 extra T-cycles; using a table indexed by signed `char`.

---

### L6 — The 8-bit ALU and flags in anger
**Difficulty** ★★ · **Depends on** L5 · **New capability** `cpu_instrs` gets past the ALU tests.

**Lecture**
- `ADD/ADC/SUB/SBC/AND/XOR/OR/CP` and their flag rules, including `AND` setting H, `OR/XOR` clearing all
  but Z, and `CP` being a subtraction that discards its result.
- `INC/DEC r8`: `INC` clears N and does *not* touch C; `DEC` sets N and does *not* touch C.
- `DAA`: decimal adjust, driven entirely by N/H/C — the reason N exists.
- Half-carry derivation: `((a & 0x0F) + (b & 0x0F) + carry) > 0x0F`; carry: `result > 0xFF`.

**Reference code**

```c
/* alu_add — the single add used by ADD, ADC, and (with complement) SUB/SBC.
 * Write the flag formulas exactly as below. Blargg's cpu_instrs fails on the
 * first approximated half-carry, so there is no "close enough" here.
 */
static uint8_t alu_add(CPU *c, uint8_t lhs, uint8_t rhs, bool carry_in)
{
    unsigned carry = (carry_in && cpu_flag(c, FLAG_C)) ? 1u : 0u;
    unsigned result = (unsigned)lhs + (unsigned)rhs + carry;

    cpu_set_flag(c, FLAG_Z, (result & 0xFFu) == 0u);   /* result is zero      */
    cpu_set_flag(c, FLAG_N, false);                    /* ADD is not subtract */
    /* Half-carry: did the low nibble carry into bit 4? */
    cpu_set_flag(c, FLAG_H, ((lhs & 0x0Fu) + (rhs & 0x0Fu) + carry) > 0x0Fu);
    /* Full carry: did the byte carry into bit 8? */
    cpu_set_flag(c, FLAG_C, result > 0xFFu);

    return (uint8_t)result;
}

/* op_daa — decimal adjust, used after ADD/ADC/SUB/SBC on packed BCD values.
 * The table is small enough to write out literally; deriving it from first
 * principles is an optional exercise that the test ROMs will grade for you.
 */
static void op_daa(CPU *c, MMU *m)
{
    (void)m;
    uint8_t correction = 0;
    bool carry = cpu_flag(c, FLAG_C);

    if (!cpu_flag(c, FLAG_N)) {           /* previous op was an addition */
        if (cpu_flag(c, FLAG_H) || (c->a & 0x0Fu) > 0x09u) correction |= 0x06u;
        if (carry || c->a > 0x99u) { correction |= 0x60u; carry = true; }
    } else {                               /* previous op was a subtraction */
        if (cpu_flag(c, FLAG_H)) correction |= 0x06u;
        if (carry) correction |= 0x60u;
    }

    c->a = (uint8_t)(c->a + (cpu_flag(c, FLAG_N) ? -correction : correction));
    cpu_set_flag(c, FLAG_Z, c->a == 0u);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry);
}
```

**Lab** Run `cpu_instrs` with the serial harness from L11 stubbed out to `gb_log` and read the first
failing test group number.

**Done when** no failing group number is below the "op r,r" and "alu" groups.

**Pitfalls** `AND` must set `H` (and clear `N`, clear `C`); `OR`/`XOR` clear all three; `INC` must not touch
`C`; `DAA` must not touch `N`.

---

### L7 — 16-bit ALU, the stack, and control flow
**Difficulty** ★★ · **Depends on** L6 · **New capability** the CPU can run loops, calls, and returns.

**Lecture**
- `ADD HL,r16`, `INC/DEC r16`, `ADD SP,e8` (signed immediate, H and C from bit 3 and bit 7 of the *low
  byte*), `LD HL,SP+e8`, `LD SP,HL`.
- Stack discipline: `PUSH` decrements SP *then* writes high, then decrements and writes low; `POP` is the
  reverse. SP must never be written as `sp - 2` in one shot if the write order matters for OAM DMA conflict tests.
- `JP/JR/CALL/RET/RETI/RST` and the conditional forms; `RETI` also sets `IME`.
- `JP (HL)` is a jump to the *address in HL*, not an indirect memory read — a classic bug.
- Cycle accounting for taken vs. not-taken conditional branches.

**Reference code**

```c
/* cpu_push16 / cpu_pop16 — the stack grows DOWN and stores the high byte
 * first. The order matters: games read their own stack, and OAM DMA conflicts
 * in L28 depend on the exact bus accesses, not just the final SP value.
 */
void cpu_push16(CPU *c, MMU *m, uint16_t v)
{
    c->sp--;
    mmu_write8(m, c->sp, (uint8_t)(v >> 8));  /* high byte at higher address */
    c->sp--;
    mmu_write8(m, c->sp, (uint8_t)(v & 0xFF));
}

uint16_t cpu_pop16(CPU *c, MMU *m)
{
    uint8_t lo = mmu_read8(m, c->sp++);
    uint8_t hi = mmu_read8(m, c->sp++);
    return (uint16_t)((hi << 8) | lo);
}

/* op_add_sp_e8 — ADD SP, e8. This is the only place the SM83 mixes signed
 * and unsigned arithmetic. H and C come from the LOW byte only, and are
 * computed on the *unsigned* interpretation of the operand.
 */
static void op_add_sp_e8(CPU *c, MMU *m)
{
    (void)m;
    uint8_t e = mmu_read8(m, c->pc++);
    unsigned r = (unsigned)(c->sp & 0xFFu) + (unsigned)e;

    cpu_set_flag(c, FLAG_Z, false);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, (r & 0x0Fu) < (c->sp & 0x0Fu));
    cpu_set_flag(c, FLAG_C, r > 0xFFu);

    /* The displacement is signed: 0x80..0xFF means -128..-1. Casting through
     * int8_t is the portable way to say that; casting to unsigned is not. */
    c->sp = (uint16_t)(c->sp + (int8_t)e);
}
```

**Done when** `cpu_instrs` reaches the "cpu control" / "jumps" groups with no failures.

**Pitfalls** `LD (HL+),A` vs `LD (HL-),A` direction; `JP (HL)`; `ADD SP,e8` flag source;
`POP AF` must mask the low nibble of F.

---

### L8 — CB prefix: rotates, shifts, SWAP, BIT/RES/SET, and DAA
**Difficulty** ★★★ · **Depends on** L7 · **New capability** the whole second opcode page.

**Lecture**
- Prefix dispatch: opcode `0xCB` fetches a second byte; the CB page has its own cycle counts.
- Rotates `RLC/RRC/RL/RR` and shifts `SLA/SRA/SRL`; `Z` from the result, `C` from the shifted-out bit,
  `N` and `H` always cleared; `SRA` preserves bit 7, `SRL` does not.
- `SWAP` clears all flags except `Z`.
- `BIT` sets `Z = !bit`, sets `H`, clears `N`, leaves `C` untouched; `RES`/`SET` change no flags at all.
- Why the CB page is the natural home of the generic operand form `CB op (HL)` with fixed 16 T-cycles.

**Reference code**

```c
/* alu_rlc — rotate left circular. The bit that falls off bit 7 becomes the
 * carry, and Z depends only on the result. Note that rotates are the ONLY
 * "ALU-like" operations that clear N and H but also set C unconditionally.
 */
static uint8_t alu_rlc(CPU *c, uint8_t v)
{
    uint8_t carry = (uint8_t)(v >> 7);
    v = (uint8_t)((v << 1) | carry);

    cpu_set_flag(c, FLAG_Z, v == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry != 0u);
    return v;
}

/* op_bit_b — BIT b, r. Sets Z to the INVERSE of the tested bit, sets H,
 * clears N, and — this is the part students break — leaves C untouched.
 * dmg_sound test 01 checks this exact interaction with the C flag.
 */
static void op_bit_b(CPU *c, MMU *m)
{
    uint8_t imm = mmu_read8(m, c->pc++);
    unsigned bit = (imm >> 3) & 0x07u;
    unsigned reg = imm & 0x07u;
    uint8_t value = cpu_read_r8(c, m, reg);

    cpu_set_flag(c, FLAG_Z, ((value >> bit) & 1u) == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, true);
    /* FLAG_C intentionally untouched. */
}
```

**Done when** `cpu_instrs` prints "Passed all tests" (with L9/L10 done) and `instr_timing` no longer
reports wildly wrong cycle counts.

**Pitfalls** `SRA` vs `SRL` on bit 7; `BIT` clobbering `C`; `SWAP` leaving `H` set; using 12 T-cycles for
`CB` ops on `(HL)` instead of 16.

---

### L9 — Timers: DIV, TIMA, TMA, TAC
**Difficulty** ★★ · **Depends on** L8 · **New capability** time passes on its own.

**Lecture**
- `DIV` (`FF04`) is the *high byte of a free-running 16-bit counter* clocked every T-cycle, so it ticks at
  16384 Hz and resets to 0 when written. It is not a counter you own.
- `TAC` (`FF07`) bits 0–1 pick which counter bit clocks `TIMA`: `00`→bit 9 (4096 Hz), `01`→bit 3 (262144 Hz),
  `10`→bit 5 (65536 Hz), `11`→bit 7 (16384 Hz). Bit 2 enables. Rate `00` means *stopped*, not slowest.
- `TIMA` overflow reloads from `TMA` and raises the timer interrupt; the reload has a 4 T-cycle delay that
  `mem_timing`/Mooneye `timing` tests observe.
- `DIV` write also resets the timer counter; the `DIV-APU` bit (bit 4 falling edge of the divider) drives the
  APU frame sequencer in L24.

**Reference code**

```c
/* timer_tick — advance the timer subsystem by one T-cycle.
 *
 * The whole trick is that DIV, TIMA's clock source, and the APU frame sequencer
 * are all derived from the SAME 16-bit counter. Modeling them as three
 * independent counters produces emulators that look right and fail mem_timing.
 */
void timer_tick(Timer *t, uint32_t t_cycles)
{
    /* Which bit of the 16-bit counter clocks TIMA? Selected by TAC bits 0-1.
     * Bit 9 -> every 1024 T-cycles (4096 Hz), and so on. */
    static const uint8_t TAC_BIT[4] = { 9, 3, 5, 7 };

    for (uint32_t i = 0; i < t_cycles; i++) {
        uint16_t before = t->div_counter;
        t->div_counter++;                     /* always free-running */

        /* Detect a falling edge of the selected bit: TIMA increments on the
         * 1 -> 0 transition, not on every T-cycle. */
        if (t->tac & 0x04u) {
            uint8_t bit = TAC_BIT[t->tac & 0x03u];
            if (((before >> bit) & 1u) && !((t->div_counter >> bit) & 1u)) {
                if (t->tima == 0xFF) {
                    t->tima = t->tma;         /* reload, and 4 T-cycles later... */
                    t->overflow_pending = 4;    /* ...raise the interrupt     */
                } else {
                    t->tima++;
                }
            }
        }

        if (t->overflow_pending && --t->overflow_pending == 0)
            if_set(t, IF_TIMER);               /* request the timer interrupt */
    }
}
```

**Done when** Blargg `instr_timing` and Mooneye `timing/div_timing`, `timing/tima_timing` pass.

**Pitfalls** storing `DIV` as a byte; incrementing `TIMA` on the wrong edge; forgetting that writing `TAC`
can *change the rate immediately* and that `TIMA` can be clocked by the write itself.

---

### L10 — Interrupts, IME, HALT, and the HALT bug
**Difficulty** ★★★ · **Depends on** L9 · **New capability** the machine services interrupts.

**Lecture**
- `IE` (`FFFF`) and `IF` (`FF0F`) bits 0–4: VBlank, STAT, Timer, Serial, Joypad; vectors at
  `0x40, 0x48, 0x50, 0x58, 0x60`.
- `IME` and `EI`'s **one-instruction delay**; `DI`; `RETI` sets IME *and* returns.
- Dispatch order: lowest bit wins when several are pending; the CPU clears the `IF` bit itself.
- Cost: pushing `PC` and jumping costs 5 M-cycles (20 T-cycles).
- `HALT`: with `IME=0` and `IE&IF != 0`, hardware does not halt but **fails to increment PC** for one
  instruction — the HALT bug. `STOP` is a separate, rarer case.

**Reference code**

```c
/* cpu_service_interrupt — called once per instruction, before the fetch.
 *
 * Two subtle behaviours live here and both are directly tested:
 *   1. HALT exits on any pending interrupt whose IE bit is set, even if IME=0,
 *      and the handler is NOT entered in that case.
 *   2. EI only takes effect after the instruction that follows it, which is why
 *      `ime_pending` exists as a separate field.
 */
bool cpu_service_interrupt(CPU *c, MMU *m)
{
    uint8_t pending = (uint8_t)(mmu_read8(m, 0xFF0F) & mmu_read8(m, 0xFFFF) & 0x1Fu);
    if (pending == 0u)
        return false;

    c->halted = false;    /* any enabled+requested interrupt wakes the CPU */
    if (!c->ime)
        return false;     /* ...but without IME the handler is not entered */

    for (unsigned bit = 0; bit < 5u; bit++) {
        if ((pending >> bit) & 1u) {
            c->ime = false;                             /* IME is cleared by hardware */
            if_set_clear(m, bit);                       /* the CPU clears the IF bit */
            cpu_push16(c, m, c->pc);                    /* push return address (16 T) */
            c->pc = (uint16_t)(0x40u + 8u * bit);       /* vector table       */
            return true;
        }
    }
    return false;
}
```

**Done when** Blargg's `cpu_instrs` prints **"Passed all tests"** over the serial harness.

**Pitfalls** `EI` taking effect immediately; `HALT` when `IME=0` and `IF&IE != 0`; interrupt priority by
lowest bit; forgetting to clear the `IF` bit; servicing interrupts before rather than after an instruction.

---

### L11 — Serial port and the headless test harness
**Difficulty** ★★ · **Depends on** L10 · **New capability** CI can grade the emulator without a human.

**Lecture**
- `SB` (`FF01`) and `SC` (`FF02`): Blargg's ROMs write a character to `SB`, then `0x81` to `SC` to start an
  internal-clock transfer; the emulator's "transfer" is to print the byte and raise the serial interrupt
  (`SC` bit 7) after the standard 8 × 512 Hz delay (or immediately, since tests do not measure it).
- Mooneye's ROMs use the same protocol; `dmg-acid2` instead writes a magic sequence to `SB` at the end.
- Headless mode: `--headless --rom test.gb --max-frames N --serial-out` and a pass/fail exit code.
- Golden-file testing: `--trace` output hashed and compared to a committed reference; frame hashes for PPU.

**Done when** `ctest` runs `cpu_instrs`, `instr_timing`, `mem_timing`, `mem_timing-2` and fails the build if
any does not print "Passed".

**Pitfalls** blocking on real wall-clock time for the serial delay; printing raw bytes instead of text; not
flushing output before exit (the last "Passed" line gets lost).

---

## Section 3 — Game loading (Weeks 6–7)

### L12 — Cartridge header: parse, validate, report
**Difficulty** ★★ · **Depends on** L11 · **New capability** the emulator identifies what it was given.

**Lecture**
- Header layout: `0100` entry point (`NOP; JP 0150`), `0104–0133` Nintendo logo (checked by the boot ROM),
  `0134–0143` title, `0143` CGB flag (`0x80` works on DMG, `0xC0` CGB-only), `0144–0145` new licensee,
  `0146` SGB flag, `0147` cart type, `0148` ROM size, `0149` RAM size, `014A` destination,
  `014C` old licensee, `014D` header checksum, `014E–014F` global checksum.
- Header checksum algorithm: `x = 0; for i in 0x134..0x14C: x = x - byte[i] - 1`; valid when `x == byte[0x14D]`.
- ROM size codes `0x00–0x08` (32 KiB → 8 MiB, 2 → 512 banks); RAM size codes `0x00–0x05`.
- Cart type table: `0x00` ROM-only, `0x01–0x03` MBC1, `0x05/0x06` MBC2, `0x0F–0x13` MBC3,
  `0x19–0x1E` MBC5, `0x0B–0x0D` MMM01, `0x20` MBC6, `0x22` MBC7, `0xFC–0xFF` camera/TAMA/HuC.
- Design rule: unknown cart type or bad checksum → refuse to run with a precise diagnostic, not a crash.

**Reference code** `src/cart.c`

```c
/* cart_parse_header — fill the CartHeader and validate the ROM.
 *
 * A header checksum failure is a hard error: it means the file is corrupt or the
 * student is looking at a headerless dump. The global checksum is informational
 * only (many commercial ROMs get it wrong), so we warn instead of failing.
 */
bool cart_parse_header(Cart *cart, const uint8_t *rom, size_t rom_size)
{
    if (rom_size < 0x150) {                 /* header must fit in the file */
        gb_log("ROM too small to contain a cartridge header: %zu bytes", rom_size);
        return false;
    }

    /* Title is 16 bytes, but on newer carts only the first 11 are the title and
     * bytes 15-16 hold the manufacturer code. Copy 16 and stop at NUL. */
    memcpy(cart->header.title, rom + 0x134, 16);
    cart->header.title[16] = '\0';
    cart->header.cgb_flag  = rom[0x143];
    cart->header.cart_type = rom[0x147];
    cart->header.rom_size  = rom[0x148];
    cart->header.ram_size  = rom[0x149];

    uint8_t checksum = 0;
    for (uint16_t addr = 0x134; addr <= 0x14C; addr++)
        checksum = (uint8_t)(checksum - rom[addr] - 1u);

    if (checksum != rom[0x14D]) {
        gb_log("bad header checksum: computed %02X, header says %02X",
                checksum, rom[0x14D]);
        return false;
    }
    return true;
}
```

**Lab** Feed the parser a deliberately corrupted ROM and a homebrew ROM; both paths must be observable.

**Done when** the emulator prints title, mapper, ROM/RAM size and CGB support for any homebrew ROM,
and refuses a corrupted one with a non-zero exit code.

**Pitfalls** title NUL-termination at offset 16; confusing the *header* checksum with the *global* one;
assuming `ram_size == 0` means "no RAM" (MBC2 has 512×4 bits built in).

---

### L13 — Bank switching: MBC1, MBC2, MBC5
**Difficulty** ★★★ · **Depends on** L12 · **New capability** ROMs larger than 32 KiB run correctly.

**Lecture**
- The banking problem: the CPU sees 32 KiB of window, the cart holds up to 8 MiB.
- Write-only registers decoded by *address range*, not by value: `0000–1FFF` RAM enable,
  `2000–3FFF` bank number, `4000–5FFF` bank high / RAM bank, `6000–7FFF` mode.
- MBC1 quirks: writing bank `0` selects `1`; 5-bit bank register; in mode 1 the `4000–5FFF`
  register contributes bits 5–6; MBC2 has 512×4 bits of internal RAM and ignores bit 4 of the bank.
- MBC5: 9-bit bank number across two registers, **bank 0 is legal** here (and many games use it).
- Why the bus must call into the cart: banking changes the *meaning* of `4000–7FFF` and `A000–BFFF`.

**Reference code**

```c
/* mbc1_write — MBC1 register writes.
 *
 * Every MBC type is just a small state machine driven by address ranges. Keeping
 * the banks as separate fields (rather than patching pointers into a ROM array)
 * is what makes save states (L29) and the debugger's memory viewer (L15) easy.
 */
static void mbc1_write(Cart *cart, uint16_t addr, uint8_t v)
{
    switch (addr >> 13) {
    case 0:  /* 0000-1FFF: RAM enable. Only the low nibble matters: 0x0A enables. */
        cart->mbc1.ram_enabled = (v & 0x0Fu) == 0x0Au;
        break;

    case 1:  /* 2000-3FFF: ROM bank number, 5 bits. */
        v &= 0x1Fu;
        if (v == 0) v = 1;                     /* quirk: bank 0 means bank 1 */
        cart->mbc1.rom_bank = (uint8_t)((cart->mbc1.rom_bank & 0x60u) | v);
        break;

    case 2:  /* 4000-5FFF: RAM bank, or ROM bits 5-6 in mode 1. */
        v &= 0x03u;
        if (cart->mbc1.mode == 0) {
            cart->mbc1.rom_bank = (uint8_t)((cart->mbc1.rom_bank & 0x1Fu) | (v << 5));
        } else {
            cart->mbc1.ram_bank = v;
        }
        break;

    case 3:  /* 6000-7FFF: banking mode. 0 = ROM, 1 = RAM. */
        cart->mbc1.mode = v & 1u;
        if (cart->mbc1.mode == 0) cart->mbc1.ram_bank = 0;  /* mode change resets RAM bank */
        break;
    }
}
```

**Done when** Mooneye's `emulator-only/mbc1/*` and `mbc5/*` ROMs pass.

**Pitfalls** bank 0 on MBC1; MBC2's 4-bit RAM; MBC5 allowing bank 0; RAM enable applying to
MBC1's *RAM only* (not ROM).

---

### L14 — MBC3, RTC, and persistence
**Difficulty** ★★★ · **Depends on** L13 · **New capability** saves survive a restart.

**Lecture**
- MBC3 registers: `0000–1FFF` RAM/RTC enable, `2000–3FFF` 7-bit ROM bank (0 → 1), `4000–5FFF` RAM bank
  `0–3` or RTC register `08–0C`, `6000–7FFF` latch clock (write `0` then `1`).
- RTC registers: `08` seconds, `09` minutes, `0A` hours, `0B` days low, `0C` day high + halt + carry.
- Battery-backed RAM: on exit write `<rom>.sav`; on load, size it from the header and validate it.
- Deterministic RTC: advance the clock from emulated cycles, never from `time()`.

**Done when** a game with battery RAM saves, the process is killed, restarted, and the save is intact;
the same ROM with a fresh save behaves like a new cartridge.

**Pitfalls** treating `6000–7FFF` as a bank register; persisting save files without a size check;
using wall-clock time for the RTC (kills L29's replay determinism).

---

## Section 4 — The debugger (Weeks 7–8)

> Design rule for the whole section: **the debugger is a separate consumer of the same core API.** It must
> not require changes to `cpu.c` beyond the tracing hooks added in L15.

### L15 — Tracing and the disassembler
**Difficulty** ★★ · **Depends on** L14 · **New capability** humans can read what the CPU did.

**Lecture**
- Reuse `OPS[256]` / `OPS_CB[256]` mnemonics from L5; a disassembler that re-derives the instruction
  encoding is a second source of truth and *will* disagree with the core.
- Decoding operands without executing them: a small `read`-free `disasm_at(mmu, addr, out)` that uses the ROM bytes.
- Register dump format (`AF BC DE HL SP PC FZ FH FC`) chosen so `diff` against a reference log is meaningful.
- Bounded trace ring buffer; never `fprintf` inside `cpu_step` (it distorts timing and hides bugs).

**Reference code**

```c
/* debug_disasm — format the instruction at `addr` into `out`.
 *
 * This function deliberately re-implements operand formatting rather than calling
 * the opcode handlers, because the handlers have side effects. It shares the
 * mnemonic strings with the core, so the two can never drift.
 */
size_t debug_disasm(MMU *m, uint16_t addr, char *out, size_t out_len)
{
    uint8_t op = mmu_read8(m, addr);
    const Instr *in = &OPS[op];

    if (op == 0xCB) {
        const Instr *cb = &OPS_CB[mmu_read8(m, (uint16_t)(addr + 1))];
        return (size_t)snprintf(out, out_len, "CB %s", cb->mnemonic);
    }
    if (!in->mnemonic)
        return (size_t)snprintf(out, out_len, "DB %02X", op);

    /* Operand bytes follow the opcode in ROM. How many depends on the
     * mnemonic's shape, which is why the table stores a `len` field too. */
    if (in->len == 1)
        return (size_t)snprintf(out, out_len, "%s", in->mnemonic);
    if (in->len == 2)
        return (size_t)snprintf(out, out_len, "%s, %02X", in->mnemonic,
                                mmu_read8(m, (uint16_t)(addr + 1)));
    uint16_t imm = (uint16_t)(mmu_read8(m, (uint16_t)(addr + 1)) |
                              (mmu_read8(m, (uint16_t)(addr + 2)) << 8));
    return (size_t)snprintf(out, out_len, "%s, %04X", in->mnemonic, imm);
}
```

**Done when** `--trace` for the first 10 000 instructions of a known ROM matches a reference log
instruction-for-instruction and cycle-for-cycle.

**Pitfalls** showing the *next* instruction rather than the executed one; omitting flags from the dump;
logging after execution (then a crash loses the faulting instruction).

---

### L16 — Breakpoints, watchpoints, and stepping
**Difficulty** ★★ · **Depends on** L15 · **New capability** targeted debugging instead of log archaeology.

**Lecture**
- Breakpoint kinds: `PC` execution breakpoints, memory read/write watchpoints, and "break on interrupt N".
- A command loop over `stdin` (`s`, `n`, `c`, `b`, `w`, `bt`, `regs`, `x/16`, `dump`) with no dependency on SDL.
- Stack unwinding from `SP` using the return addresses on the stack.
- `gdb`/`lldb` integration: `handle SIGINT` + an `int3`-style trap on the `PC` breakpoint, so students
  can use their real debugger on the emulator.
- Headless scriptability: `--debug-script script.txt` for automated repro cases.

**Done when** the students can set a breakpoint on the VBlank vector `0x40`, run, and print `PC`, `SP`, and
the top three stack return addresses when it fires.

**Pitfalls** watchpoints that fire on the emulator's own internal reads; breakpoints that miss because the
address is checked before rather than after the fetch; no way to disable the trace at a breakpoint (log spam).

---

### L17 — Hardware-state viewers: VRAM, OAM, tiles, palettes, memory
**Difficulty** ★★★ · **Depends on** L16 · **New capability** the debugger shows the *machine*, not just the CPU.

**Lecture**
- Tile viewer: decode `8000–97FF` into 8×8 tiles with the DMG palette so rendering bugs become visible
  *before* the PPU exists (this lesson can run ahead of Section 5).
- BG map viewer with tile indices; OAM viewer with sprite positions and the selected line's sprite list.
- Palette viewer for `BGP`/`OBP0`/`OBP1`; I/O register viewer from the L4 table; memory hexdump with region labels.
- Rendering these viewers with SDL as an extra "debug window", or with a text UI — the lesson's point is that the
  viewers consume the same core API as the game loop, with no privileged access.

**Done when** the tile viewer, pointed at a real ROM's VRAM, shows the same font/graphics a human recognizes.

**Pitfalls** viewers that mutate state; viewers that read `vram` directly rather than through `mmu_read8`, hiding
banking bugs (L30).

---

### L18 — Frame diffing and automated regression tests
**Difficulty** ★★★ · **Depends on** L17 · **New capability** an automated check for visual correctness.

**Lecture**
- Deterministic inputs → deterministic frames: the same ROM, the same button script, the same frame count must
  produce the same 32-bit hash.
- Frame hashing (FNV-1a over the framebuffer), committed golden hashes, and CI failure on mismatch.
- Screenshot export to PNG (via `SDL_SaveBMP` or stb_image_write) for human review and the capstone report.
- Why golden frames are acceptable here but *not* as a substitute for hardware test ROMs: golden frames catch
  regressions, test ROMs catch wrongness.

**Done when** CI fails when a deliberate one-line PPU bug is introduced and passes when it is reverted.

**Pitfalls** hashing before the frame is complete; using a non-deterministic RTC/audio in the hash path;
committing golden frames generated by a broken build.

---

## Section 5 — Screen and input (Weeks 9–10)

> Design rule: the PPU is a **state machine driven by T-cycles**, and the framebuffer is an *output*, not the
> model. Drawing the framebuffer once per frame (rather than per scanline, during mode 3) passes `dmg-acid2`
> only by luck and fails all timing-sensitive games.

### L19 — PPU registers, LCDC, STAT, and the mode machine
**Difficulty** ★★★ · **Depends on** L18 · **New capability** the PPU has its own clock and interrupts.

**Lecture**
- `LCDC` (`FF40`) bit by bit: 7 LCD enable, 6 window map, 5 window enable, 4 BG/window tile data,
  3 BG map, 2 OBJ size, 1 OBJ enable, 0 BG/window enable.
- `STAT` (`FF41`): mode bits 0–1, LYC=LY bit 2, mode 0/1/2 interrupts bits 3–5, LYC interrupt bit 6.
- Timing: 456 dots per line × 154 lines = **70224 T-cycles** per frame. Mode 2 = 80 dots,
  mode 3 = 172–289 dots (longer with more sprites), mode 0 = the rest, mode 1 = lines 144–153.
- VBlank interrupt fires at the start of line 144. `LY` (`FF44`) is read-only; `LYC` (`FF45`) is writable.
- The host frame is **70224 T-cycles whatever LCDC does**: with the LCD on, VBlank ends it; with
  the LCD off (or turned off mid-frame, as a screen transition does) the PPU can never reach VBlank,
  so the CPU clock ends it. A frame that only ends on VBlank therefore reports a spurious overrun
  whenever a game switches the LCD off.
- Line 153 quirk and why "STAT reads during mode 3" is the classic wrong answer.

**Reference code**

```c
/* ppu_tick — advance the PPU by `t_cycles`.
 *
 * The PPU is a 4-state machine per scanline, not a "render everything at the
 * end of the frame" function. Every mode transition is observable by the CPU via
 * STAT, and games poll STAT to time their VRAM writes.
 */
void ppu_tick(PPU *p, MMU *m, uint32_t t_cycles)
{
    if (!(p->lcdc & LCDC_LCD_ENABLE)) {
        /* LCD off: LY resets to 0 and the PPU consumes no time. */
        p->dot = 0;
        p->ly = 0;
        p->mode = PPU_MODE_HBLANK;
        return;
    }

    p->dot += (int)t_cycles;

    switch (p->mode) {
    case PPU_MODE_OAM:                      /* mode 2: 80 dots */
        if (p->dot >= 80) {
            p->mode = PPU_MODE_TRANSFER;
            p->dot -= 80;
            if (p->ly < 144) ppu_render_scanline(p, m, p->ly);  /* L20/L21 */
        }
        break;

    case PPU_MODE_TRANSFER:                 /* mode 3: 172-289 dots */
        if (p->dot >= 172) {                /* use a real length once L28 demands it */
            p->mode = PPU_MODE_HBLANK;
            p->dot -= 172;
            if (p->stat & STAT_MODE0_IRQ) if_set(m, IF_STAT);
        }
        break;

    case PPU_MODE_HBLANK:                   /* mode 0: rest of the line */
        if (p->dot >= 204) {                /* 456 - 80 - 172 */
            p->dot -= 204;
            p->ly++;
            ppu_check_lyc(p, m);            /* LYC=LY coincidence interrupt */
            if (p->ly == 144) {
                p->mode = PPU_MODE_VBLANK;
                if_set(m, IF_VBLANK);       /* games start their frame here */
            } else {
                p->mode = PPU_MODE_OAM;
            }
        }
        break;

    case PPU_MODE_VBLANK:                   /* mode 1: lines 144-153 */
        if (p->dot >= 456) {
            p->dot -= 456;
            p->ly++;
            if (p->ly > 153) {              /* frame complete: wrap to line 0 */
                p->ly = 0;
                p->frame_ready = true;
                p->mode = PPU_MODE_OAM;
            }
        }
        break;
    }
}
```

**Done when** a homebrew ROM's `STAT` polling loops stop hanging, and `LY` advances at 59.7 lines/ms.

**Pitfalls** rendering at end-of-frame; mode 3 always 172 dots; `LY` writable; VBlank interrupt
raised at line 153 rather than 144; STAT interrupt raised *while* `STAT` is being read; letting the frame
length depend on LCDC, so a game that turns the LCD off mid-frame logs a spurious overrun.

---

### L20 — Background and window rendering
**Difficulty** ★★★ · **Depends on** L19 · **New capability** actual pixels.

**Lecture**
- Tile data `8000–97FF`, tile maps `9800–9BFF` and `9C00–9BFF`; LCDC bit 4 chooses the
  `8800` signed addressing mode (`0x9000` + `(int8_t)index * 16`), which students always get wrong.
- Tile row format: two bytes per row, low plane then high plane; pixel colour index =
  `((high >> (7 - x)) & 1) << 1 | ((low >> (7 - x)) & 1)`.
- Scrolling with `SCX`/`SCY` and the `x + SCX`, `y + SCY` wrap at 256; `BGP` palette lookup.
- Window: `WX`/`WY`, only when `WX <= 166` and `WY <= 143`; `WX - 7` is the left edge; the window's
  internal line counter advances on lines where the window is on and **resets while it is off**; a game that
  turns the window off and on mid-frame (a HUD that appears partway down) relies on that reset, or it starts
  on the wrong tile row. The window takes priority over BG.
- `LCDC` bit 0 = BG/window enable: when clear, the BG and window are blanked to white (this is how
  many games hide a status bar).

**Reference code**

```c
/* ppu_bg_pixel — fetch one BG/window pixel.
 *
 * Called once per pixel from ppu_render_scanline. Everything here is pure
 * address arithmetic on VRAM; there is no "current pixel" state stored anywhere,
 * which is what makes partial-frame rendering (L28) possible later.
 */
static uint8_t ppu_bg_pixel(PPU *p, MMU *m, int x, int y, bool window)
{
    uint16_t map_base = window ? (p->lcdc & LCDC_WIN_MAP ? 0x9C00 : 0x9800)
                              : (p->lcdc & LCDC_BG_MAP  ? 0x9C00 : 0x9800);

    /* In window mode the coordinates are window-relative; in BG mode they are
     * screen coordinates plus the scroll registers. */
    uint8_t px = window ? (uint8_t)(x - (p->wx - 7)) : (uint8_t)(x + p->scx);
    uint8_t py = window ? (uint8_t)p->win_line       : (uint8_t)(y + p->scy);

    uint16_t map_addr = (uint16_t)(map_base + (py / 8) * 32 + (px / 8));
    uint8_t index = mmu_read8_vram(m, map_addr, 0);      /* bank 0 always */

    /* LCDC bit 4 = 0 means signed tile indices based at 0x9000. The offset is
     * relative to 0x9000, so index 0 is tile at 0x9000 and index 0x80 wraps to
     * 0x8800 — this is the "signed addressing" students forget. */
    uint16_t tile_addr;
    if (p->lcdc & LCDC_TILE_DATA) {
        tile_addr = (uint16_t)(0x8000 + index * 16);
    } else {
        tile_addr = (uint16_t)(0x9000 + (int8_t)index * 16);
    }

    uint8_t lo = mmu_read8_vram(m, (uint16_t)(tile_addr + 2 * (py % 8)), 0);
    uint8_t hi = mmu_read8_vram(m, (uint16_t)(tile_addr + 2 * (py % 8) + 1), 0);
    unsigned bit = 7u - (unsigned)(px % 8);
    uint8_t colour = (uint8_t)(((hi >> bit) & 1u) << 1 | ((lo >> bit) & 1u));

    /* BGP holds four 2-bit shades; index 0 is transparent for BG priority
     * purposes even though it still draws a colour. */
    return (uint8_t)((p->bgp >> (colour * 2)) & 0x03u);
}
```

**Done when** the BG map of a homebrew ROM matches a reference screenshot; the window opens and closes
exactly where the game expects.

**Pitfalls** tile-index signed mode; window internal line counter; `WX` not offset by 7; window drawn
when `WY` is off-screen; forgetting that BG and window share the `BGP` palette.

---

### L21 — Sprites, priority, 8×16 mode, and OAM DMA
**Difficulty** ★★★★ · **Depends on** L20 · **New capability** games look like themselves.

**Lecture**
- OAM: 40 entries × 4 bytes (`Y`, `X`, `tile`, `attr`). `Y=0` and `X=0` mean *hidden*, and the stored
  coordinates are both offset when drawn (`X` by 8, `Y` by 16).
- 10 sprites per scanline; DMG selects the first 10 in OAM order then sorts by `X`; ties go to the lower
  OAM index. CGB selects the 10 lowest `X` then sorts by OAM index (the difference matters in L30).
- Attribute bits: 7 BG-over-OBJ priority, 6 Y flip, 5 X flip, 4 palette, 3–0 CGB VRAM bank.
- 8×16 mode: `tile` bit 0 is ignored and the two tiles are `tile & 0xFE` then `| 1`.
- Transparency: colour index 0 is transparent for sprites; sprite-vs-BG priority is per-pixel and only applies
  when the BG colour index is non-zero.
- OAM DMA: writing `FF46 = (src >> 8)` copies 160 bytes from `src & 0xFF00` in 160 M-cycles; the CPU can only
  read/write HRAM during the transfer (games rely on this to prepare the next frame).

**Done when** `dmg-acid2` renders pixel-identically to the reference image, including the sprite-limits and
sprite-priority test areas.

**Pitfalls** sprite `Y=0`/`X=0`; 8×16 tile numbering; ties broken by `X` rather than OAM index; DMG
"first 10 in OAM order" vs CGB "lowest X"; OAM DMA not blocking the bus.

---

### L22 — SDL presentation: scaling, pacing, screenshots
**Difficulty** ★★ · **Depends on** L21 · **New capability** students can *see* their emulator.

**Lecture**
- Framebuffer ownership: the PPU writes `uint32_t fb[144][160]`; the host uploads with `SDL_UpdateTexture`.
- Integer scaling (4×, 8×) with nearest-neighbour; aspect-ratio letterboxing; fullscreen and DPI.
- Frame pacing at 59.7275 Hz with vsync; optional frame skip; frame-time histogram on screen.
- Screenshot export (`SDL_SaveBMP`) for the frame-diff tests from L18 and the capstone report.
- Optional: palette selection (DMG green, greyscale, CGB), and an on-screen FPS/cycles counter.

**Done when** the emulator displays the L21 BG/window/sprite image at 60 fps on a 4K display with no tearing.

**Pitfalls** uploading the framebuffer to a texture allocated per frame; using linear filtering; scaling the
texture rather than the destination rect; blocking the main thread on `SDL_RenderPresent` with vsync off.

---

### L23 — Input: the joypad matrix and SDL mapping
**Difficulty** ★★ · **Depends on** L22 · **New capability** the emulator is playable.

**Lecture**
- `FF00` JOYP: bit 5 selects buttons, bit 4 selects directions, both active-low; bits 0–3 read the selected
  group active-low (0 = pressed); bits 6–7 unused and read 1.
- Interrupt: the joypad interrupt (bit 4 of `IF`) fires on a *high-to-low* transition of any selected line — so
  it fires when a button is *pressed*, and only if that group is currently selected.
- Games poll `JOYP` in a tight loop; a correct model must return the *current* state, not a latched event.
- SDL mapping: keyboard (arrows + `Z`/`X`/`Enter`/`Backspace`) and gamepad (`SDL_OpenGamepad`, d-pad, face
  buttons, Start/Select); rebinding stored in a small config file.

**Reference code**

```c
/* joypad_write — the game selects which half of the matrix it wants to read.
 *
 * This is the entire input protocol. There is no "input buffer": the game reads
 * JOYP whenever it wants, so we must recompute the low nibble on every write and
 * on every read, from the current SDL state.
 */
void joypad_write(Joypad *j, uint8_t v)
{
    j->select = v & 0x30u;               /* bits 4-5 select a group */
    joypad_refresh(j);
}

/* joypad_refresh — recompute bits 0-3 from the currently selected group. */
static void joypad_refresh(Joypad *j)
{
    uint8_t pressed = 0;
    if (!(j->select & 0x20u)) pressed |= j->buttons;      /* 0 = pressed */
    if (!(j->select & 0x10u)) pressed |= j->dpad;
    j->value = (uint8_t)(0xC0u | j->select | (pressed & 0x0Fu));
}
```

**Done when** a game's title screen responds to Start within one frame; the joypad interrupt is raised exactly
once per press (verified with a watchpoint on `IF`).

**Pitfalls** latching input into a queue instead of exposing live state; raising the interrupt on release; forgetting
that unselected groups read 0xF; treating `FF00` as read-only.

---

## Section 6 — Sound (Weeks 11–12)

> Design rule: **the APU generates one sample per T-cycle at 1.048576 MHz** (CPU ÷ 4) and the host downsamples.
> Generating "per frame" or "per scanline" produces the classic wrong-pitch, wrong-envelope audio.

### L24 — APU architecture, frame sequencer, and the SDL audio path
**Difficulty** ★★★ · **Depends on** L23 · **New capability** the emulator makes a sound at all.

**Lecture**
- Register file `FF10–FF3F`: `NR10` sweep, `NR11–NR14` square 1, `NR21–NR24` square 2, `NR30–NR34` wave,
  `NR41–NR44` noise, `NR50` master volume, `NR51` panning, `NR52` power/status.
- `NR52` bit 7 powers the APU down; bits 0–3 are *channel status* and are read-only, computed from the channels.
- Frame sequencer: one step per 512 Hz (8192 T-cycles), 8 steps. Length counters clocked on steps
  0,2,4,6 (256 Hz); sweep on steps 2 and 6 (128 Hz); envelope on step 7 (64 Hz). Step 7 also latches.
- The sequencer is driven by the falling edge of `DIV` bit 4 (bit 12 of the 16-bit counter) — same counter as L9.
- SDL3 audio: request a stream at 48 kHz, `SDL_OpenAudioDeviceStream`, feed 16-bit stereo frames; the SDL
  stream performs the resampling, so the emulator's own resampler can stay simple.
- Buffer policy: a lock-free ring buffer between the emulation thread and SDL; if empty, output silence and count
  the underrun instead of blocking.

**Reference code**

```c
/* apu_tick — advance the APU by one T-cycle.
 *
 * The frame sequencer is clocked by a falling edge of DIV bit 4, i.e. every 8192
 * T-cycles. Because DIV is the shared timer counter from L9, the APU and the
 * timer are physically the same clock; modeling them separately is the classic
 * source of "sound drifts by a few percent" bugs.
 */
void apu_tick(APU *a, uint32_t t_cycles)
{
    for (uint32_t i = 0; i < t_cycles; i++) {
        /* Advance every enabled channel by one T-cycle. Channels run at
         * different rates internally (the square period is in 2-T-cycle units),
         * which is why they must be ticked per T-cycle, not per sample. */
        if (a->nr52 & 0x80u) {
            square_tick(&a->sq1, a);
            square_tick(&a->sq2, a);
            wave_tick(&a->wave, a);
            noise_tick(&a->noise, a);
        }

        /* Frame sequencer: 512 Hz = every 8192 T-cycles. */
        if (++a->fs_div >= 8192) {
            a->fs_div = 0;
            apu_frame_sequencer_step(a);
        }

        /* Downsample to the host rate. Adding SAMPLE_RATE per T-cycle and
         * emitting whenever the accumulator passes the CPU clock gives exactly
         * SAMPLE_RATE samples per emulated second with integer arithmetic,
         * so the audio never drifts against the video. */
        a->sample_acc += (uint32_t)SAMPLE_RATE;
        while (a->sample_acc >= CPU_CLOCK_HZ) {
            a->sample_acc -= CPU_CLOCK_HZ;
            apu_emit_sample(a);              /* push into the ring buffer */
        }
    }
}
```

**Done when** `dmg_sound` test `01-registers` passes and a single square channel produces a steady tone with no
clicks or dropouts over 60 seconds.

**Pitfalls** generating audio per frame; a 32-bit accumulator overflow at 4.19 MHz; `NR52` power-off not silencing
channels; blocking the emulation thread on the audio queue.

---

### L25 — Square 1 (sweep) and square 2
**Difficulty** ★★★★ · **Depends on** L24 · **New capability** the emulator plays melodies.

**Lecture**
- Period → frequency: `f = 131072 / (2048 - period)`, where `period = NR13 | ((NR14 & 7) << 8)`. Sweep
  and length operate on that period register, not on the frequency.
- Duty waveforms 12.5/25/50/75 % from `NR11` bits 6–7; `NR11` bits 0–5 are the length load.
- Envelope: `NR12` bits 7–4 initial volume, bit 3 direction, bits 2–0 period (0 = off, otherwise n × 1/64 s).
- Sweep: `NR10` period (bits 6–4), direction (bit 3), shift (bits 2–0). Shift 0 → no change; the
  overflow check must use the *new* period and must be re-evaluated on each sweep step; sweep triggers the
  "obscure" behaviour when it overflows the 11-bit period.
- DAC: `NR12` bits 7–3 all zero disables the channel and forces its output to 0 regardless of the timer.
- Trigger (`NR14` bit 7) reloads the period and, if the length counter is 0, reloads it from `NR11`.

**Done when** `dmg_sound` tests `02-len ctr`, `03-trigger`, `04-sweep` pass and a sweep-enabled tone rises in pitch
audibly and by the right ratio.

**Pitfalls** `f = 131072 / (2048 - period)` off by one; sweep using the old period; envelope clocking on the
wrong step; the DAC disabling rule; forgetting that the length counter is only reloaded if it is 0.

---

### L26 — Wave and noise channels
**Difficulty** ★★★★ · **Depends on** L25 · **New capability** every channel type.

**Lecture**
- Wave RAM `FF30–FF3F`, 16 bytes = 32 4-bit samples. `NR32` bits 5–6 select 100 %/50 %/25 %, and 0 mutes.
  Sample position does not reset on trigger; it resets on power, and the "wave read while playing" quirk (DMG only)
  is exactly what `dmg_sound` test 09 checks.
- Wave channel DAC off (`NR30` bit 7 clear) silences the channel and must also disable the channel's length.
- Noise: LFSR 15 bits, `NR43` divisor code `r` and shift `s`; the divisor is `r`, except that code 0
  means `0.5` (it is neither silent nor a divide by zero). `f = 524288 / (divisor * 2^(shift+1))` Hz, so one
  LFSR shift lasts `(divisor << shift) * 16` T-cycles; `NR43` bit 3 width mode (7-bit mode taps bit 6 rather
  than bit 14); the LFSR must be reset on trigger. A divisor that is 16× too large plays the noise four octaves
  flat, i.e. a hi-hat becomes a rumble.
- `NR44` bit 6 is the length enable; length counters for wave/noise follow the same 256 Hz clock from L24.

**Done when** `dmg_sound` tests `05-sweep details`, `06-overflow on trigger`, `07-len sweep period sync`, `08-len ctr during power`,
`09-wave read while on`, `10-wave trigger while on`, `11-regs after power`, `12-wave write while on` pass.

**Pitfalls** wave sample position reset on trigger; LFSR reset; 7-bit vs 15-bit LFSR mode; noise divisor 0
meaning 0.5; wave RAM read while the channel is playing.

---

### L27 — Mixing, master volume, filtering, and resampling
**Difficulty** ★★★★ · **Depends on** L26 · **New capability** audio that sounds correct, not just present.

**Lecture**
- `NR50` master volume (bits 0–2 right, 4–6 left), `NR51` panning (bits 0–3 right ch1–4, 4–7 left ch1–4);
  channels are summed per side with per-channel DAC output in the range 0–15.
- DC offset: every DAC output has a constant offset; without a high-pass filter the mix has a large DC step when
  channels start, which is the classic "thump" that `dmg_sound` test 12 detects. Model the capacitor as
  `out = in - last_in + 0.996 * last_out` (the standard approximation).
- Aliasing: apply a low-pass filter before downsampling, or oversample and average, to avoid the harsh buzz.
- Bit depth conversion: 4-bit DAC → float → 16-bit signed for SDL, with saturation.
- Verification: compare a recorded waveform against a real Game Boy recording of the same test ROM; FFT to check harmonics.

**Done when** all of `dmg_sound` passes, and a 60-second run has no underruns, no DC thump, and no audible
clicks at channel boundaries.

**Pitfalls** mixing with floating point per sample (slow and drift-prone); applying the high-pass to the
mixed signal *before* per-channel volume; resampling with a naive `int` ratio; blocking in the audio callback.

---

## Section 7 — Accuracy, extras, and everything else (Weeks 13–14)

### L28 — The accuracy ladder
**Difficulty** ★★★★★ · **Depends on** L27 · **New capability** timing-correct emulation.

**Lecture**
- Define three tiers and grade accordingly: **functional** (games run), **timing-correct** (Mooneye `acceptance`
  passes), **cycle-accurate** (sub-instruction and PPU-dot level).
- PPU mode 3 length depends on sprites and the window on that line; getting it wrong breaks games that
  write to VRAM in HBlank.
- DMA bus conflicts: the CPU cannot access `0000–FF7F` during OAM DMA, and cannot access anything but HRAM during
  CGB HDMA. Mooneye `dma` tests check this.
- `HALT` bug; `STOP` and double speed switching (L30); the `EI` delay interacting with `HALT`.
- Interrupt timing vs. `STAT` mode changes: an interrupt that lands in mode 3 can miss the mode-0 window.

**Done when** Mooneye `acceptance/*` passes at the "timing-correct" tier, and the documented remaining failures
are listed with a reason in the capstone report (honest failure lists earn full credit; silent failures do not).

**Pitfalls** "fixing" a test by special-casing it; changing the frame loop to wall-clock; adding `SDL_Delay` inside
`cpu_step`.

---

### L29 — Save states, rewind, and deterministic replay
**Difficulty** ★★★★ · **Depends on** L28 · **New capability** reproducibility.

**Lecture**
- Serialization: every subsystem needs `serialize(FILE*)`/`deserialize(FILE*)` with an explicit format version;
  a save state is exactly `CPU + MMU + PPU + APU + Timer + Cart(MBC state) + Joypad`.
- What must *not* be serialized: host pointers, SDL objects, open file handles, the ROM image (store a hash instead).
- Rewind: keep a ring buffer of compressed save states (delta or full + zlib); memory budget and cadence.
- Replay: record the button bitmask per frame; deterministic emulation (L28) makes a replay reproduce a bug exactly.
- Test: hash a replay's final framebuffer and assert it is stable across builds and machines.

**Done when** `F5` saves, `F7` loads, and a bug reproduced from a replay survives a `git checkout` to the
commit that introduced it.

**Pitfalls** serializing pointers; forgetting to serialize the MBC bank registers; rewind that stores
*input* rather than *state* and drifts.

---

### L30 — CGB support: speed, banks, colour, HDMA
**Difficulty** ★★★★★ · **Depends on** L29 · **New capability** colour games run.

**Lecture**
- `KEY1` (`FF4D`): bit 7 reads the current speed, bit 0 requests a switch; the switch happens on the `STOP`
  that follows. Double speed halves CPU cycles per frame in *T-cycle* terms: the CPU executes twice as many
  instructions per frame, while the PPU/APU clocks are unchanged. Get this wrong and everything desyncs.
- `VBK` (`FF4F`) VRAM bank, `SVBK` (`FF70`) WRAM bank (0 → 1); banked VRAM means `mmu_read8_vram(addr, bank)`
  must take the bank as a parameter (as in L20's code), not read a global.
- Palettes: `BGPI`/`BGPD` (`FF68`/`FF69`) and `OBPI`/`OBPD` (`FF6A`/`FF6B`), auto-increment bit 7,
  8 BG and 8 OBJ palettes of 4 colours × 2 bytes RGB555 little-endian.
- `HDMA1–HDMA5` (`FF51–FF55`): general-purpose DMA (16 bytes per write) and HBlank DMA (16 bytes per
  HBlank, `HDMA5` bit 7 reads 0 while busy), plus the source/destination masks.
- DMG-compatibility: `CGB` flag `0x80` vs `0xC0`; the same game must render correctly in both modes
  (DMG mode uses `BGP`/`OBP0`/`OBP1`, CGB mode uses the colour palettes).
- Sprite priority differs from DMG (L21) — selection of the 10 sprites per line by lowest `X`.

**Done when** a CGB homebrew ROM runs in colour, `dmg-acid2` still passes in DMG mode, and the CGB variant
of the acid test passes.

**Pitfalls** double-speed clock applied to the PPU as well; `SVBK` 0 meaning bank 0; palette index
auto-increment not applied; HDMA running while the LCD is off; mixing DMG and CGB sprite selection.

---

### L31 — Serial port, link cable, and accessories
**Difficulty** ★★★★ · **Depends on** L30 · **New capability** two instances talk.

**Lecture**
- Serial: `SB`/`SC`, internal vs external clock, 8 × 512 Hz bit shift, and the two-player `SC` handshake.
- Two emulator instances over a local socket or shared memory; the point is the *timing handshake*, not the transport.
- MBC3 RTC (L14) as a "wall clock" accessory; MBC7 accelerometer, HuC3, camera, and why `0xFC–0xFF` exist.
- Infrared (`RP`, `FF56`) and the `RP` register on CGB.

**Done when** two instances of the emulator exchange a byte with both running at 59.7 fps without either
stalling, and a link-cable homebrew demo runs.

**Pitfalls** assuming the internal clock always wins; not synchronizing the two instances' frame boundaries;
serial interrupt timing.

---

## Section 8 — Capstone (Weeks 15–16)

### L32 — Capstone: accuracy report, polish, packaging
**Difficulty** ★★★ · **Depends on** L31 · **New capability** a shippable artifact.

**Deliverables**
1. **Accuracy report**: table of every test suite run, pass/fail, and for each failure an honest root cause.
   A failure with a correct explanation earns more than a silent pass.
2. **Performance report**: frame time breakdown (CPU, PPU, APU, SDL), profiles before/after one optimization, and
   the effect on determinism.
3. **Release artifact**: CMake install target, `--help`, config file, key rebinding, save directory, version string,
   README with build instructions, and CI on three platforms.
4. **Demo**: a game running on the emulator, a save state round-trip, a rewind, and a replay reproducing a bug.
5. **Optional stretch**: WebAssembly build via Emscripten; a TAS-style input file; a shader-based
   colour-correct upscaler; a mobile build.

**Grading note**: the report is graded on the honesty and specificity of the failure list and on whether the
described bug reproduces. "All tests pass" with no list is treated as incomplete evidence.

---

## Appendix A — Test-ROM matrix

| Lesson | ROM / suite | What it proves |
|---|---|---|
| L6–L10 | Blargg `cpu_instrs` | instruction semantics, flags, control flow |
| L9 | Blargg `instr_timing`, Mooneye `timing` | cycle counts, DIV/TIMA edges |
| L11 | Blargg `mem_timing`, `mem_timing-2` | bus cycle counts, read/write order |
| L11 | Mooneye `acceptance/` | interrupt and timing corner cases |
| L12 | homebrew + corrupted ROM | header validation |
| L13 | Mooneye `emulator-only/mbc1`, `mbc2`, `mbc5` | banking, bank 0, RAM enable |
| L14 | MBC3 + battery homebrew | persistence, RTC |
| L18 | golden frame hashes (own) | regression detection |
| L21 | `dmg-acid2` | BG/window/sprite priority, 10-sprite limit |
| L21 | Mealybug Tearoom tests | sprite priority, window edge cases |
| L24–L27 | Blargg `dmg_sound` 01–12 | APU registers, length, envelope, sweep, wave, noise |
| L28 | Mooneye `acceptance`, `emulator-only` | timing-correct tier |
| L30 | `dmg-acid2` CGB variant, CGB homebrew | colour, HDMA, banking |

Every suite is public, free, and redistributable. Students download them; the repo does not vendor ROMs.

---

## Appendix B — Hardware quick-reference (post-boot DMG)

| Item | Value |
|---|---|
| CPU clock | 4 194 304 Hz |
| Frame | 154 lines × 456 dots = 70 224 T-cycles (59.7275 Hz) |
| Interrupt vectors | 0x40 VBlank, 0x48 STAT, 0x50 Timer, 0x58 Serial, 0x60 Joypad |
| Interrupt cost | 5 M-cycles (20 T-cycles) |
| DIV | high byte of a 16-bit counter at 16 384 Hz |
| TAC rates | 4096 / 262144 / 65536 / 16384 Hz (bits 9 / 3 / 5 / 7) |
| OAM DMA | 160 bytes from `src & 0xFF00`, 160 M-cycles, HRAM-only access |
| APU sample clock | 1 048 576 Hz (CPU ÷ 4) |
| Frame sequencer | 512 Hz; length 256 Hz (0,2,4,6); sweep 128 Hz (2,6); envelope 64 Hz (7) |
| DMG sprite rule | first 10 in OAM order, then sorted by X; tie → lower OAM index |
| CGB sprite rule | 10 lowest X, then sorted by OAM index |
| Wave RAM | 16 bytes = 32 samples of 4 bits |
| Noise LFSR | 15-bit, taps bit 0 and bit 1; 7-bit mode taps bit 6 |
| Post-boot registers (DMG) | AF 01B0, BC 0013, DE 00D8, HL 014D, SP FFFE, PC 0100 |
| Post-boot registers (CGB) | AF 1180, BC 0000, DE FF56, HL 000D, SP FFFE, PC 0100 |

---

## Appendix C — References

**Primary**
- Pan Docs — https://gbdev.io/pandocs/ (the specification used throughout).
- The Game Boy: Complete Technical Reference (GBEDG) — https://gekkio.fi/files/gb-docs/gbctr.pdf
- Game Boy CPU (SM83) Instruction Set — https://gbdev.io/gb-opcodes/
- The Cycle-Accurate Game Boy Docs (AntonioND) — PPU/DMA timing.
- Game Boy CPU Manual (Nintendo/Sharp) — original instruction descriptions, including DAA.
- "The Ultimate Game Boy Talk" — Michael Steil, 33C3.

**Peer emulators to read *after* your own implementation works**
- SameBoy (C, MIT) — accuracy reference for PPU/APU corner cases.
- binjgb (C, MIT) — compact, readable reference for PPU and mappers.
- Gearboy / Gambatte — mapper and APU reference.

**Test suites**
- retrio/gb-test-roms (Blargg) — `cpu_instrs`, `instr_timing`, `mem_timing`, `dmg_sound`, `halt_bug`.
- Gekkio/mooneye-gb — `acceptance/`, `emulator-only/`.
- mattcurrie/dmg-acid2 — PPU rendering reference.
- mattcurrie/mealybug-tearoom-tests — sprite/window edge cases.
- Gekkio's `gb-qa` — one-stop runner for the above.

**Tooling**
- `mgba` / `SameBoy` for A/B comparison on the same ROM.
- `rgbds` assembler for writing the students' own test ROMs (a great capstone stretch goal).

---

## Appendix D — Portability and style rules for the codebase

1. **No `#include <SDL.h>` in `cpu.c`, `mmu.c`, `cart.c`, `ppu.c`, `apu.c`, `timer.c`.** The core logs
   through a `gb_log(fmt, ...)` macro that the host implements. This is what makes L11 headless and L32's
   WASM port possible.
2. **Fixed-width types everywhere** (`uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`); never `int` for a hardware
   value; never `char` for a byte.
3. **No undefined behaviour**: no unaligned pointer casts, no signed overflow, no `memcpy` of overlapping ranges,
   no shifting by ≥ the width. `-fsanitize=address,undefined` must be clean.
4. **Endianness**: the register unions in `cpu.h` rely on little-endian layout; Appendix E gives the portable accessor
   form for big-endian hosts.
5. **Timing is data**: T-cycle counts live in the instruction table and in `ppu_tick`/`apu_tick`, never in
   `SDL_Delay` or `clock()`.
6. **Determinism**: no `rand()`, no `time()`, no thread scheduling inside the core. Threads are allowed only for
   moving audio buffers in the host layer.
7. **One owner per subsystem**: only `ppu.c` writes `framebuffer`; only `mmu.c` touches the region arrays; the
   debugger reads through `mmu_read8`, never directly.

## Appendix E — Portable register access (for big-endian hosts)

```c
/* If the union layout in cpu.h is unacceptable (big-endian host, or a strict
 * aliasing policy), use accessors instead. The cost is a couple of shifts per
 * access; the benefit is well-defined behaviour on every host. */
static inline uint16_t cpu_get_bc(const CPU *c)
{
    return (uint16_t)(((uint16_t)c->b << 8) | c->c);
}

static inline void cpu_set_bc(CPU *c, uint16_t v)
{
    c->b = (uint8_t)(v >> 8);
    c->c = (uint8_t)(v & 0xFF);
}
```
