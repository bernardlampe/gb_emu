# Game Boy Emulator: High-Level Theory

Scope: the conceptual model behind a DMG Game Boy emulator, using the module split
already present in this repo (`cpu.c`, `mmu.c`, `ppu.c`, `apu.c`, `timer.c`,
`joypad.c`, `cart.c`, `gb.c`).

---

## 1. The core idea: one deterministic state machine

A Game Boy emulator is a pure function:

```
(state, input) -> (state, output)
```

driven by a clock. Everything else is implementation detail. **Determinism is the
whole game**: if the machine is a function of cycle count and inputs, then bugs are
reproducible, save states are a `memcpy` of one struct, and tests can assert exact
cycle counts.

The hardware has one master clock at **4.194304 MHz**. The CPU's "machine cycle"
(M-cycle) is 4 T-cycles. The PPU counts *dots*, which are also T-cycles. The
natural design is therefore: **T-cycles are the single unit of time**, and every
peripheral is a function of a global cycle counter.

## 2. Two synchronization strategies

- **Instruction-stepped**: run one CPU instruction, get its cycle cost, then advance
  every peripheral by that many cycles. Simple, fast, and sufficient for roughly 95%
  of commercial games.
- **Cycle-stepped**: advance one T-cycle at a time; the CPU itself becomes a small
  state machine (fetch/decode/execute phases). Required for exact memory-timing
  tests, HDMA, and mid-instruction PPU/DMA interactions.

## 3. CPU (Sharp SM83 / LR35902)

State: `A F B C D E H L`, `SP`, `PC`, `IME`, `halted`, plus the HALT-bug latch.
Flags occupy the high nibble of `F`: Z, N, H, C.

The work is a 256-entry base opcode table plus a 256-entry `CB`-prefixed table.
Correctness traps, roughly in order of how often they bite:

- **Half-carry rules differ by operation.** Bit 3→4 for 8-bit adds, bit 11→12 for
  16-bit adds (`ADD HL,rr`). `DAA` adjusts `A` based on `N`/`H`/`C` to produce BCD.
- **Conditional and branch instructions have two cycle costs** (taken vs not taken);
  taken 16-bit jumps cost more.
- **`HALT` with `IME=0` and a pending interrupt** triggers the HALT bug: the next
  byte is not skipped and `PC` fails to increment.
- **`EI` is delayed by one instruction.** Interrupts are serviced after the *next*
  instruction completes, not immediately.
- **Interrupt dispatch costs 5 M-cycles** (20 T-cycles) and pushes `PC`.

Interrupts: `IE` at `0xFFFF`, `IF` at `0xFF0F`. Vectors: `0x40` VBlank,
`0x48` STAT, `0x50` Timer, `0x58` Serial, `0x60` Joypad. Priority is by lowest
vector address. An interrupt fires when `IE & IF & 0x1F` is nonzero and `IME` is set;
the handler clears the corresponding `IF` bit and jumps.

## 4. MMU / bus

One 64 KiB address space with every device attached to it. This seam is what makes
the system testable: **all device interaction goes through `read8`/`write8`**. The
CPU never touches VRAM or the APU directly.

Mapping theory:

| Range           | Device                                   |
| --------------- | ---------------------------------------- |
| `0x0000–0x3FFF` | Cartridge ROM, bank 0 (fixed)             |
| `0x4000–0x7FFF` | Cartridge ROM, switchable bank            |
| `0x8000–0x9FFF` | VRAM                                     |
| `0xA000–0xBFFF` | Cartridge RAM                            |
| `0xC000–0xDFFF` | WRAM                                     |
| `0xE000–0xFDFF` | Echo RAM (mirror of `0xC000–0xDDFF`)      |
| `0xFE00–0xFE9F` | OAM                                      |
| `0xFEA0–0xFEFF` | Unusable                                 |
| `0xFF00–0xFF7F` | I/O registers                            |
| `0xFF80–0xFFFE` | HRAM                                     |
| `0xFFFF`        | `IE`                                     |

Unmapped reads return `0xFF` (open bus); unmapped writes are dropped. Getting
the *unmapped* behavior right matters more than it sounds — games probe it.

## 5. Cartridges and MBCs

The header at `0x0100–0x014F` holds the Nintendo logo (validated by the boot ROM),
the title, the CGB flag, and the **cartridge type byte**, which selects the memory
bank controller: none, MBC1, MBC2, MBC3, MBC5.

Banking is performed by **writes into the ROM address range** — writing a value to
`0x2000–0x3FFF` selects a ROM bank, and so on. Battery-backed carts persist RAM
to a `.sav` file. MBC3 additionally exposes an RTC. A large fraction of
per-game compatibility lives here, so expect this module to grow.

## 6. Timers

Registers: `DIV` (`0xFF04`), `TIMA` (`0xFF05`), `TMA` (`0xFF06`), `TAC` (`0xFF07`).

The key insight: `DIV` is not a counter — it is the **upper 8 bits of a
free-running 16-bit internal counter**. `TAC` selects which bit of that counter
increments `TIMA`. On `TIMA` overflow, `TMA` is reloaded after a one-cycle delay
and the timer interrupt is raised. Writing `DIV` resets the entire internal counter,
which can produce a spurious `TIMA` increment — a classic test-ROM failure.

## 7. PPU

Per scanline: 456 dots. Mode 2 (OAM scan) → Mode 3 (drawing) → Mode 0 (HBlank).
Lines 0–143 are visible; lines 144–153 are Mode 1 (VBlank). Total: 154 lines,
**70224 dots per frame**.

Registers: `LCDC` (`0xFF40`), `STAT` (`0xFF41`), `SCY`/`SCX`, `LY`/`LYC`,
`BGP`/`OBP0`/`OBP1`, `WX`/`WY`.

Rendering theory, in the order that matters:

1. **Background** — 32×32 tile map; tiles addressed via `0x8000` or `0x8800` mode
   selected by `LCDC` bit 4; per-pixel palette lookup through `BGP`.
2. **Window** — same as background, but with its own internal line counter, enabled
   when `WY`/`WX` conditions are met.
3. **Sprites** — 8×8 or 8×16, sorted by X then by OAM index, **10 per scanline
   maximum**, behind-BG priority via `LCDC` bit 0, per-sprite palette via
   `OBP0`/`OBP1`.

Two standard simplifications: render the entire scanline at once when entering Mode 3,
and emit mode transitions afterward. The `STAT` interrupt and the `LY=LYC`
comparison are what most test ROMs actually check. VRAM/OAM access restrictions per
mode exist but only need enforcement to the extent games notice.

## 8. APU

Four channels: two square (channel 1 has a frequency sweep), one wave, one noise.
A **frame sequencer at 512 Hz** divides down into length counters (~256 Hz), volume
envelopes (~64 Hz), and sweep (~128 Hz). Registers are `NR10`–`NR52`.

Output theory: mix the four channels into a ring buffer at the emulated sample rate,
then resample to the host rate (44.1/48 kHz) in the host layer. Keeping
mix-then-resample split is what keeps `gbcore` host-independent — the same reason
`CMakeLists.txt` keeps SDL out of `gbcore`.

## 9. Joypad and serial

`P1`/`JOYP` at `0xFF00` has two strobe bits selecting the d-pad row or the button
row. Bits are **active-low**, and the joypad interrupt fires on a high→low transition.
The link cable lives in the serial registers: `SB` (`0xFF01`) and `SC` (`0xFF02`).
Link support is optional.

## 10. Host loop

Run **70224 dots per frame**, then blit and poll input, capped near 59.7 Hz. Audio
needs separate handling because the host buffer drains at its own rate; decoupling it
with a ring buffer is what prevents pitch drift.

## 11. Boot ROM

The real boot ROM scrolls the Nintendo logo and validates the header checksum. Two
options: emulate it exactly (it is a small 256-byte program) or skip it by
pre-setting the post-boot register state. Exact emulation is the more honest option
and costs little.

## 12. The accuracy ladder

The practical roadmap, and the way to know you are done:

1. Boot ROM passes.
2. `blargg cpu_instrs` passes — instruction semantics and flags.
3. `instr_timing` passes — per-instruction cycle counts.
4. `mem_timing` passes — bus access timing.
5. `dmg-acid2` / PPU tests pass — PPU correctness.
6. `mooneye` tests pass — the nasty edges: HALT bug, `DIV` reset, interrupt
   timing, DMA.

Each rung is a strictly harder class of bug, and each forces a more accurate
internal model. For the shortest path to a believable emulator: make rungs 1–3 solid,
then treat 4–6 as a long tail.

## 13. Design principles that follow

- **One bus function.** All devices sit behind it; nothing bypasses it.
- **Peripherals own their state; the bus routes to them.** No peripheral reaches
  into another's registers.
- **Cycle accounting is the single source of truth.** Save states then reduce to a
  plain struct snapshot.
- **The host layer only does I/O.** Rendering, audio resampling, and input mapping
  live outside `gbcore`.

---

In one sentence: a deterministic, cycle-driven state machine, with a single bus as
the synchronization point, and an accuracy ladder that tells you which parts of the
model are worth making exact.
