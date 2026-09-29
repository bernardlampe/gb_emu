/* debug.h — the debugger: tracing, breakpoints, viewers, REPL (L15-L18).
 *
 * The debugger is a CONSUMER of the core's public API. It reads memory through
 * mmu_read8(), disassembles with the CPU's own instruction table, and never
 * mutates hardware state. That rule is what keeps the core honest and the
 * debugger honest at the same time.
 */
#ifndef GB_DEBUG_H
#define GB_DEBUG_H

#include "gbdefs.h"

#define DEBUG_TRACE_MAX 4096   /* instructions kept in the trace ring */
#define DEBUG_TRACE_LEN 128    /* characters per trace line */
#define DEBUG_BP_MAX 64
#define DEBUG_WP_MAX 64

typedef struct GB GB;
typedef struct MMU MMU;

typedef struct {
    uint16_t addr;
    bool     enabled;
    uint64_t hits;
} DebugPoint;

typedef struct {
    bool        trace_enabled;
    DebugPoint  bp[DEBUG_BP_MAX];
    DebugPoint  wp[DEBUG_WP_MAX];
    bool        wp_on_write[DEBUG_WP_MAX];

    /* Fast-path flags. `attached` is set once any debugger feature is armed
     * (tracing, a breakpoint, a watchpoint or the REPL) and stays set: until
     * then debug_record() must not format or store anything, or every
     * instruction of ordinary play pays for a trace nobody reads. `wp_count` lets
     * debug_watch() skip the watchpoint scan when none is armed. */
    bool     attached;
    unsigned wp_count;

    /* Trace ring buffer: record only while the debugger is attached, so a crash
     * can be examined after the fact, and print only when --trace was given. */
    char     trace[DEBUG_TRACE_MAX][DEBUG_TRACE_LEN];
    unsigned trace_head;
    uint64_t trace_count;

    bool     break_hit;
    uint16_t break_addr;
} Debug;

void debug_init(Debug *d);
void debug_trace_enable(Debug *d, bool on);

/* debug_attach — the host calls this once it has installed the breakpoint and
 * watchpoint tables, so the core knows the debugger is observing. Without it the
 * per-instruction trace ring is skipped and the emulator runs at full speed. */
void debug_attach(Debug *d);

/* debug_record — called by gb_run_instruction() before each instruction:
 * appends to the trace ring and stops at an execution breakpoint. */
void debug_record(GB *gb);

/* debug_watch — called by the bus on every read/write when watchpoints exist. */
void debug_watch(Debug *d, uint16_t addr, bool write);

void debug_serialize(Debug *d, FILE *f);
void debug_deserialize(Debug *d, FILE *f);

/* debug_disasm — format the instruction at addr into out. Reuses the CPU's
 * instruction table, so the disassembly can never disagree with the core. */
size_t debug_disasm(MMU *m, uint16_t addr, char *out, size_t out_len);
void   debug_dump_trace(GB *gb, FILE *out);
void   debug_dump_regs(GB *gb, FILE *out);

/* Viewers (L17). All of them are read-only. */
void debug_dump_tiles(GB *gb, FILE *out);   /* decoded 8x8 tiles as text */
void debug_dump_oam(GB *gb, FILE *out);
void debug_dump_io(GB *gb, FILE *out);
void debug_dump_vram(GB *gb, FILE *out);    /* hexdump of both banks */
void debug_dump_mem(GB *gb, uint16_t addr, unsigned len, FILE *out);

/* debug_repl — interactive command loop on stdin (L16). Commands:
 *   s [n]        step n instructions (default 1)
 *   c            continue until a breakpoint hits or the ROM halts
 *   b ADDR       toggle an execution breakpoint (hex address)
 *   w ADDR       toggle a read/write watchpoint
 *   regs         dump registers
 *   x ADDR [LEN]  hexdump memory
 *   dis [ADDR]   disassemble 16 instructions
 *   tiles        dump decoded tiles
 *   oam          dump the sprite table
 *   io           dump the I/O registers
 *   trace [on|off]
 *   help, quit
 */
void debug_repl(GB *gb);

#endif /* GB_DEBUG_H */
