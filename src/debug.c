/* debug.c — the debugger: tracing, breakpoints, viewers and the REPL (L15-L18).
 *
 * The debugger is a CONSUMER of the core's public API and nothing more:
 *   - it reads memory through mmu_read8() / mmu_read_vram(), never through the
 *     MMU's arrays directly, so it sees exactly what the CPU sees (open bus, DMA
 *     lockout, CGB banks and all);
 *   - it disassembles with the CPU's own OPS[] / OPS_CB[] tables, so the printed
 *     mnemonic can never drift from what the core actually executes;
 *   - it mutates hardware state ONLY through gb_run_instruction() and the
 *     breakpoint/watchpoint tables. Viewers are read-only.
 *
 * This file must not include <SDL.h> or call SDL_Log: it is core code and uses
 * gb_log() like every other core module.
 *
 * Reference implementation for CS 4XX "System Emulation".
 */
#include "cpu.h"   /* forward-declares MMU, which debug.h's prototypes need */
#include "debug.h"

#include "gb.h"
#include "mmu.h"
#include "ppu.h"

#include <inttypes.h>   /* PRIu64 for uint64_t */
#include <stddef.h>     /* offsetof */
#include <string.h>     /* memset, strcmp */

/* ======================================================================
 * 0. Small helpers
 * ====================================================================== */

/* Debug is embedded by value inside GB (see gb.h), so the owning GB sits at a
 * fixed negative offset from the Debug pointer. debug_watch() is called by the bus
 * with &gb->debug and needs the CPU's PC for its message, but Debug deliberately
 * has no back-pointer field: this recovers the owner without adding one. */
static GB *debug_owner(Debug *d)
{
    return (GB *)(void *)((char *)d - offsetof(GB, debug));
}

/* Advance addr by n bytes, wrapping in 16 bits. */
static uint16_t addr_add(uint16_t addr, unsigned n)
{
    return (uint16_t)(addr + n);
}

/* One hexdump row's worth of decoding. `use_vram` selects the banked VRAM
 * accessor, which is the only way the debugger is allowed to see bank 1. */
static uint8_t peek(GB *gb, uint16_t addr, bool use_vram, unsigned bank)
{
    if (use_vram) return mmu_read_vram(&gb->mmu, addr, bank);
    return mmu_read8(&gb->mmu, addr);
}

/* hexdump — classic address / 16 hex bytes / ASCII layout, 16 bytes per row. */
static void hexdump(GB *gb, uint16_t addr, unsigned len, FILE *out,
                    bool use_vram, unsigned bank)
{
    for (unsigned i = 0; i < len; i += 16u) {
        uint16_t base = addr_add(addr, i);

        fprintf(out, "%04X:", base);

        /* Hex column: pad the last row so the ASCII column stays aligned. */
        for (unsigned j = 0; j < 16u; j++) {
            if (i + j >= len) {
                fputs("   ", out);
                continue;
            }
            fprintf(out, " %02X", peek(gb, addr_add(base, j), use_vram, bank));
        }

        fputs("  ", out);

        /* ASCII column: printable bytes as themselves, the rest as '.'. */
        for (unsigned j = 0; j < 16u && i + j < len; j++) {
            uint8_t v = peek(gb, addr_add(base, j), use_vram, bank);
            fputc((v >= 0x20u && v < 0x7Fu) ? (int)v : (int)'.', out);
        }

        fputc('\n', out);
    }
}

/* Names for the documented I/O registers, so debug_dump_io() is readable. */
static const char *io_name(uint8_t reg)
{
    switch (reg) {
    case 0x00: return "P1/JOYP";
    case 0x01: return "SB";
    case 0x02: return "SC";
    case 0x04: return "DIV";
    case 0x05: return "TIMA";
    case 0x06: return "TMA";
    case 0x07: return "TAC";
    case 0x0F: return "IF";
    case 0x40: return "LCDC";
    case 0x41: return "STAT";
    case 0x42: return "SCY";
    case 0x43: return "SCX";
    case 0x44: return "LY";
    case 0x45: return "LYC";
    case 0x46: return "DMA";
    case 0x47: return "BGP";
    case 0x48: return "OBP0";
    case 0x49: return "OBP1";
    case 0x4A: return "WY";
    case 0x4B: return "WX";
    case 0x4D: return "KEY1";
    case 0x4F: return "VBK";
    case 0x50: return "BOOT";
    case 0x51: return "HDMA1";
    case 0x52: return "HDMA2";
    case 0x53: return "HDMA3";
    case 0x54: return "HDMA4";
    case 0x55: return "HDMA5";
    case 0x68: return "BCPS";
    case 0x69: return "BCPD";
    case 0x6A: return "OCPS";
    case 0x6B: return "OCPD";
    case 0x70: return "SVBK";
    case 0x76: return "PCM12";
    case 0x77: return "PCM34";
    default:
        /* 0x10-0x3F are the APU's NRxx sound registers. */
        if (reg >= 0x10u && reg <= 0x3Fu) return "NRxx";
        return NULL;
    }
}

/* refresh_points — recompute the fast-path flags after the point tables change.
 * Defined after debug_init(); forward-declared here because the toggles below use
 * it. */
static void refresh_points(Debug *d);

/* Toggle an execution breakpoint: disable it if one is already armed at addr,
 * re-enable it if it exists but is disabled, otherwise claim a free slot. */
static void toggle_bp(Debug *d, uint16_t addr)
{
    for (unsigned i = 0; i < (unsigned)DEBUG_BP_MAX; i++) {
        if (d->bp[i].enabled && d->bp[i].addr == addr) {
            d->bp[i].enabled = false;
            refresh_points(d);
            return;
        }
    }
    for (unsigned i = 0; i < (unsigned)DEBUG_BP_MAX; i++) {
        if (d->bp[i].addr == addr) {
            d->bp[i].enabled = true;
            refresh_points(d);
            return;
        }
    }
    for (unsigned i = 0; i < (unsigned)DEBUG_BP_MAX; i++) {
        /* A slot is free when it has never been armed: no hits, no address. */
        if (!d->bp[i].enabled && d->bp[i].hits == 0u) {
            d->bp[i].addr = addr;
            d->bp[i].enabled = true;
            refresh_points(d);
            return;
        }
    }
}

/* Same toggle policy as toggle_bp(), but for memory watchpoints. The
 * wp_on_write[] flag records the direction the watchpoint was armed for. */
static void toggle_wp(Debug *d, uint16_t addr)
{
    for (unsigned i = 0; i < (unsigned)DEBUG_WP_MAX; i++) {
        if (d->wp[i].enabled && d->wp[i].addr == addr) {
            d->wp[i].enabled = false;
            refresh_points(d);
            return;
        }
    }
    for (unsigned i = 0; i < (unsigned)DEBUG_WP_MAX; i++) {
        if (d->wp[i].addr == addr) {
            d->wp[i].enabled = true;
            refresh_points(d);
            return;
        }
    }
    for (unsigned i = 0; i < (unsigned)DEBUG_WP_MAX; i++) {
        if (!d->wp[i].enabled && d->wp[i].hits == 0u) {
            d->wp[i].addr = addr;
            d->wp[i].enabled = true;
            d->wp_on_write[i] = true;   /* arm as a write watchpoint */
            refresh_points(d);
            return;
        }
    }
}

/* Print `count` instructions starting at addr. Length comes from the same table
 * the CPU decodes with, so the walk can never misalign with execution. */
static void disasm_range(GB *gb, uint16_t addr, unsigned count, FILE *out)
{
    for (unsigned i = 0; i < count; i++) {
        char text[64];
        debug_disasm(&gb->mmu, addr, text, sizeof(text));
        fprintf(out, "%04X: %s\n", addr, text);

        /* 0xCB is a two-byte prefix; everything else takes its length from
         * the unprefixed table. A zero length would loop forever, so floor it. */
        uint8_t op = mmu_read8(&gb->mmu, addr);
        unsigned len = (op == 0xCBu) ? 2u : (unsigned)OPS[op].len;
        if (len == 0u) len = 1u;
        addr = addr_add(addr, len);
    }
}

static void repl_help(FILE *out)
{
    fputs("commands:\n"
          "  s [n]        step n instructions (default 1)\n"
          "  c            continue until a breakpoint or halt\n"
          "  b ADDR       toggle an execution breakpoint\n"
          "  w ADDR       toggle a watchpoint\n"
          "  regs         dump registers\n"
          "  x ADDR [LEN] hexdump memory (default 16 bytes)\n"
          "  dis [ADDR]   disassemble 16 instructions\n"
          "  tiles        dump decoded tiles\n"
          "  oam          dump the sprite table\n"
          "  io           dump the I/O registers\n"
          "  vram         hexdump both VRAM banks\n"
          "  trace [on|off]\n"
          "  help, quit\n", out);
}

/* Continue until a breakpoint or a halt. Bounded so a ROM that never stops
 * (or a disabled breakpoint) cannot hang the debugger forever. */
static void repl_continue(GB *gb)
{
    Debug *d = &gb->debug;

    /* If we are standing ON an enabled breakpoint, step over it once so that
     * `c` makes progress instead of re-entering this loop immediately. */
    d->break_hit = false;
    for (unsigned i = 0; i < (unsigned)DEBUG_BP_MAX; i++) {
        if (d->bp[i].enabled && d->bp[i].addr == gb->cpu.pc) {
            d->bp[i].enabled = false;
            gb_run_instruction(gb);
            d->bp[i].enabled = true;
            break;
        }
    }

    const uint64_t limit = 100000000ull;   /* 100 million instructions */
    uint64_t i;
    for (i = 0; i < limit; i++) {
        if (gb->cpu.halted) break;
        gb_run_instruction(gb);
        if (d->break_hit) break;   /* debug_record() already opened the REPL */
    }

    if (i == limit)
        fprintf(stdout, "continue: no breakpoint after %" PRIu64 " instructions\n", limit);
    else if (d->break_hit)
        fprintf(stdout, "continue: breakpoint hit at %04X\n", d->break_addr);
    else
        fputs("continue: CPU halted\n", stdout);
}

/* ======================================================================
 * 1. Lifecycle
 * ====================================================================== */

void debug_init(Debug *d)
{
    /* Zero every field: no trace, no breakpoints, empty ring. d->gb (the owning
     * machine) is not part of the struct, so nothing here can clobber it. */
    memset(d, 0, sizeof(*d));
}

/* refresh_points — recompute the fast-path flags after the point tables change.
 *
 * `attached` is sticky: once a debugger feature has been armed it stays set, so a
 * crash later in the run can still be examined. */
static void refresh_points(Debug *d)
{
    d->wp_count = 0;
    for (unsigned i = 0; i < (unsigned)DEBUG_WP_MAX; i++)
        if (d->wp[i].enabled) d->wp_count++;

    if (d->trace_enabled || d->wp_count > 0u) d->attached = true;
    for (unsigned i = 0; i < (unsigned)DEBUG_BP_MAX && !d->attached; i++)
        if (d->bp[i].enabled) d->attached = true;
}

void debug_attach(Debug *d)
{
    refresh_points(d);
}

void debug_trace_enable(Debug *d, bool on)
{
    /* Switching tracing off does NOT flush the ring: the last instructions before
     * a crash are exactly what a student wants to inspect afterwards. */
    d->trace_enabled = on;
    if (on) refresh_points(d);
}

/* ======================================================================
 * 2. Recording and the execution breakpoints (L15-L16)
 * ====================================================================== */

void debug_record(GB *gb)
{
    Debug *d = &gb->debug;

    /* Ordinary play has no debugger observing: leave before the disassembly and
     * the two formatted lines below, which together cost more than the emulation
     * itself. A breakpoint can only be armed while attached (see
     * refresh_points()), so skipping the check here cannot miss a hit. */
    if (!d->attached) return;

    /* Disassemble BEFORE the instruction runs: if the machine crashes on this
     * instruction, the trace still names it instead of the one after. */
    char text[64];
    debug_disasm(&gb->mmu, gb->cpu.pc, text, sizeof(text));

    char line[DEBUG_TRACE_LEN];
    snprintf(line, sizeof(line),
             "PC:%04X AF:%04X BC:%04X DE:%04X HL:%04X SP:%04X %c%c%c%c %s",
             gb->cpu.pc, gb->cpu.af, gb->cpu.bc, gb->cpu.de, gb->cpu.hl,
             gb->cpu.sp,
             /* Flags rendered as their letter when set, '-' when clear. */
             cpu_flag(&gb->cpu, FLAG_Z) ? 'Z' : '-',
             cpu_flag(&gb->cpu, FLAG_N) ? 'N' : '-',
             cpu_flag(&gb->cpu, FLAG_H) ? 'H' : '-',
             cpu_flag(&gb->cpu, FLAG_C) ? 'C' : '-',
             text);

    /* Copy into the current ring slot, then advance the head. The ring records
     * while the debugger is attached, so a crash can be examined after the fact. */
    snprintf(d->trace[d->trace_head], DEBUG_TRACE_LEN, "%s", line);
    d->trace_head = (d->trace_head + 1u) % DEBUG_TRACE_MAX;
    d->trace_count++;

    if (d->trace_enabled) {
        fputs(line, stdout);
        fputc('\n', stdout);
    }

    /* Execution breakpoints are checked before the instruction executes, so the
     * machine is still standing exactly where the student asked. */
    for (unsigned i = 0; i < (unsigned)DEBUG_BP_MAX; i++) {
        if (!d->bp[i].enabled || d->bp[i].addr != gb->cpu.pc) continue;

        d->bp[i].hits++;
        d->break_hit = true;
        d->break_addr = gb->cpu.pc;

        debug_dump_regs(gb, stdout);
        fprintf(stdout, "%04X: %s\n", gb->cpu.pc, text);

        /* Hand control to the student; `quit` returns and the instruction then
         * executes normally. */
        debug_repl(gb);
    }
}

void debug_watch(Debug *d, uint16_t addr, bool write)
{
    if (!d) return;

    /* Ordinary play has no watchpoint armed: leave before scanning the table,
     * which would otherwise run on every single memory access. */
    if (d->wp_count == 0u) return;

    for (unsigned i = 0; i < (unsigned)DEBUG_WP_MAX; i++) {
        if (!d->wp[i].enabled || d->wp[i].addr != addr) continue;

        d->wp[i].hits++;

        GB *gb = debug_owner(d);
        fprintf(stdout, "watchpoint: %s %04X PC:%04X\n",
                write ? "write" : "read", addr, gb->cpu.pc);

        /* Let the student inspect the machine at the instant of the access. */
        debug_repl(gb);
    }
}

/* ======================================================================
 * 3. Disassembler (L15)
 * ====================================================================== */

size_t debug_disasm(MMU *m, uint16_t addr, char *out, size_t out_len)
{
    uint8_t op = mmu_read8(m, addr);
    const Instr *in = &OPS[op];

    if (op == 0xCBu) {
        /* The CB page is a separate table with its own (two-byte) encoding. */
        const Instr *cb = &OPS_CB[mmu_read8(m, addr_add(addr, 1u))];
        return (size_t)snprintf(out, out_len, "CB %s", cb->mnemonic);
    }

    /* A NULL mnemonic means the opcode has no name: show it as raw data. */
    if (!in->mnemonic)
        return (size_t)snprintf(out, out_len, "DB %02X", op);

    /* The table's `len` says how many operand bytes follow the opcode. */
    if (in->len == 1u)
        return (size_t)snprintf(out, out_len, "%s", in->mnemonic);
    if (in->len == 2u)
        return (size_t)snprintf(out, out_len, "%s, %02X", in->mnemonic,
                                mmu_read8(m, addr_add(addr, 1u)));

    /* Three-byte form: a little-endian 16-bit immediate. */
    uint16_t imm = (uint16_t)(mmu_read8(m, addr_add(addr, 1u)) |
                             (mmu_read8(m, addr_add(addr, 2u)) << 8));
    return (size_t)snprintf(out, out_len, "%s, %04X", in->mnemonic, imm);
}

/* ======================================================================
 * 4. Viewers (L17)
 * ====================================================================== */

void debug_dump_regs(GB *gb, FILE *out)
{
    const CPU *c = &gb->cpu;
    fprintf(out,
            "AF:%04X BC:%04X DE:%04X HL:%04X SP:%04X PC:%04X "
            "IME:%d HALT:%d HALTBUG:%d T:%" PRIu64 "\n",
            c->af, c->bc, c->de, c->hl, c->sp, c->pc,
            c->ime ? 1 : 0, c->halted ? 1 : 0, c->halt_bug ? 1 : 0,
            c->t_cycles);
}

void debug_dump_trace(GB *gb, FILE *out)
{
    const Debug *d = &gb->debug;

    /* Only the last DEBUG_TRACE_MAX instructions are retained; print them
     * oldest-first by walking back from the head. */
    unsigned n = (d->trace_count < (uint64_t)DEBUG_TRACE_MAX)
                     ? (unsigned)d->trace_count
                     : (unsigned)DEBUG_TRACE_MAX;
    unsigned start = (d->trace_head + (unsigned)DEBUG_TRACE_MAX - n)
                     % (unsigned)DEBUG_TRACE_MAX;

    for (unsigned i = 0; i < n; i++) {
        unsigned idx = (start + i) % (unsigned)DEBUG_TRACE_MAX;
        fprintf(out, "%6u  %s\n", i, d->trace[idx]);
    }
}

void debug_dump_tiles(GB *gb, FILE *out)
{
    /* Colour -> character, index is the 2-bit colour number. */
    static const char shade[4] = { ' ', '.', '+', '#' };

    /* Tile data lives at 8000-97FF: 384 tiles of 16 bytes. Bank 1 only exists
     * (and is only meaningful) on CGB hardware. */
    unsigned banks = gb->cgb ? 2u : 1u;

    for (unsigned bank = 0; bank < banks; bank++) {
        fprintf(out, "VRAM bank %u\n", bank);

        for (unsigned t = 0; t < 384u; t++) {
            uint16_t base = addr_add(0x8000u, t * 16u);
            fprintf(out, "tile %03X\n", t);

            /* Each tile is 8 rows; each row is 2 bytes: low plane then high. */
            for (unsigned row = 0; row < 8u; row++) {
                uint8_t lo = mmu_read_vram(&gb->mmu, addr_add(base, row * 2u), bank);
                uint8_t hi = mmu_read_vram(&gb->mmu, addr_add(base, row * 2u + 1u), bank);

                for (unsigned x = 0; x < 8u; x++) {
                    unsigned bit = 7u - x;   /* bit 7 is the leftmost pixel */
                    unsigned colour = (unsigned)(((hi >> bit) & 1u) << 1 |
                                               ((lo >> bit) & 1u));
                    fputc(shade[colour], out);
                }
                fputc('\n', out);
            }
        }
    }
}

void debug_dump_oam(GB *gb, FILE *out)
{
    for (unsigned i = 0; i < 40u; i++) {
        uint16_t a = addr_add(0xFE00u, i * 4u);
        uint8_t y    = mmu_read8(&gb->mmu, a);
        uint8_t x    = mmu_read8(&gb->mmu, addr_add(a, 1u));
        uint8_t tile = mmu_read8(&gb->mmu, addr_add(a, 2u));
        uint8_t attr = mmu_read8(&gb->mmu, addr_add(a, 3u));

        /* A sprite with Y=0 or X=0 lies off-screen and is never drawn. */
        fprintf(out, "%2u Y:%3u X:%3u tile:%02X attr:%02X%s\n",
                i, y, x, tile, attr, (y == 0u || x == 0u) ? " (hidden)" : "");
    }
}

void debug_dump_io(GB *gb, FILE *out)
{
    /* Read every register through the bus so the printed value includes the
     * register's behaviour (read-only bits, open bus, APU power state, ...). */
    for (unsigned reg = 0; reg < 0x80u; reg++) {
        uint16_t addr = addr_add(0xFF00u, reg);
        uint8_t v = mmu_read8(&gb->mmu, addr);
        const char *name = io_name((uint8_t)reg);

        if (name)
            fprintf(out, "FF%02X %02X %s\n", reg, v, name);
        else
            fprintf(out, "FF%02X %02X\n", reg, v);
    }
}

void debug_dump_vram(GB *gb, FILE *out)
{
    /* Both banks, via the banked VRAM accessor: dumping the raw array would
     * hide banking bugs, which is the whole point of the CGB bank register. */
    for (unsigned bank = 0; bank < VRAM_BANKS; bank++) {
        fprintf(out, "VRAM bank %u\n", bank);
        hexdump(gb, 0x8000u, VRAM_BANK_SIZE, out, true, bank);
    }
}

void debug_dump_mem(GB *gb, uint16_t addr, unsigned len, FILE *out)
{
    /* Plain memory: the CPU's own view, open bus and DMA lockout included. */
    hexdump(gb, addr, len, out, false, 0);
}

/* ======================================================================
 * 5. The REPL (L16)
 * ====================================================================== */

void debug_repl(GB *gb)
{
    Debug *d = &gb->debug;
    char line[256];

    /* Entering the REPL is what "attached" means: from here on the trace ring
     * records, so a crash can be examined after the fact. */
    d->attached = true;

    for (;;) {
        /* Prompt and flush so a piped script sees it before it feeds input. */
        fputs("> ", stdout);
        fflush(stdout);

        if (!fgets(line, (int)sizeof(line), stdin)) break;   /* EOF */

        char cmd[16];
        if (sscanf(line, "%15s", cmd) != 1) continue;   /* blank line */

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
            break;
        } else if (strcmp(cmd, "s") == 0) {
            unsigned long n = 1;
            if (sscanf(line, "%*s %lu", &n) != 1) n = 1;
            if (n == 0ul) n = 1ul;
            for (unsigned long i = 0; i < n; i++) gb_run_instruction(gb);
        } else if (strcmp(cmd, "c") == 0) {
            repl_continue(gb);
        } else if (strcmp(cmd, "b") == 0) {
            unsigned a;
            if (sscanf(line, "%*s %x", &a) == 1) toggle_bp(d, (uint16_t)a);
            else fputs("usage: b ADDR\n", stdout);
        } else if (strcmp(cmd, "w") == 0) {
            unsigned a;
            if (sscanf(line, "%*s %x", &a) == 1) toggle_wp(d, (uint16_t)a);
            else fputs("usage: w ADDR\n", stdout);
        } else if (strcmp(cmd, "regs") == 0) {
            debug_dump_regs(gb, stdout);
        } else if (strcmp(cmd, "x") == 0) {
            unsigned a = gb->cpu.pc;
            unsigned len = 16;
            (void)sscanf(line, "%*s %x %u", &a, &len);
            debug_dump_mem(gb, (uint16_t)a, len, stdout);
        } else if (strcmp(cmd, "dis") == 0) {
            unsigned a = gb->cpu.pc;
            (void)sscanf(line, "%*s %x", &a);
            disasm_range(gb, (uint16_t)a, 16u, stdout);
        } else if (strcmp(cmd, "tiles") == 0) {
            debug_dump_tiles(gb, stdout);
        } else if (strcmp(cmd, "oam") == 0) {
            debug_dump_oam(gb, stdout);
        } else if (strcmp(cmd, "io") == 0) {
            debug_dump_io(gb, stdout);
        } else if (strcmp(cmd, "vram") == 0) {
            debug_dump_vram(gb, stdout);
        } else if (strcmp(cmd, "trace") == 0) {
            char arg[8];
            if (sscanf(line, "%*s %7s", arg) == 1)
                debug_trace_enable(d, strcmp(arg, "on") == 0);
            else
                debug_trace_enable(d, !d->trace_enabled);   /* no arg: toggle */
        } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) {
            repl_help(stdout);
        } else {
            fprintf(stdout, "unknown command: %s (try help)\n", cmd);
        }
    }
}

/* ======================================================================
 * 6. Save states (L29)
 * ====================================================================== */

void debug_serialize(Debug *d, FILE *f)
{
    /* Only the debugger's own configuration is saved: the trace ring is a
     * transient artefact, not part of the machine's state. */
    GB_SER(d->trace_enabled, f);
    GB_SER(d->trace_count, f);

    for (unsigned i = 0; i < (unsigned)DEBUG_BP_MAX; i++) {
        GB_SER(d->bp[i].addr, f);
        GB_SER(d->bp[i].enabled, f);
        GB_SER(d->bp[i].hits, f);
    }

    for (unsigned i = 0; i < (unsigned)DEBUG_WP_MAX; i++) {
        GB_SER(d->wp[i].addr, f);
        GB_SER(d->wp[i].enabled, f);
        GB_SER(d->wp[i].hits, f);
        GB_SER(d->wp_on_write[i], f);
    }
}

void debug_deserialize(Debug *d, FILE *f)
{
    GB_DESER(d->trace_enabled, f);
    GB_DESER(d->trace_count, f);

    for (unsigned i = 0; i < (unsigned)DEBUG_BP_MAX; i++) {
        GB_DESER(d->bp[i].addr, f);
        GB_DESER(d->bp[i].enabled, f);
        GB_DESER(d->bp[i].hits, f);
    }

    for (unsigned i = 0; i < (unsigned)DEBUG_WP_MAX; i++) {
        GB_DESER(d->wp[i].addr, f);
        GB_DESER(d->wp[i].enabled, f);
        GB_DESER(d->wp[i].hits, f);
        GB_DESER(d->wp_on_write[i], f);
    }

    /* The loaded tables may differ from the ones in memory, so recompute the
     * fast-path flags; a state with a point armed must keep recording. */
    refresh_points(d);
}
