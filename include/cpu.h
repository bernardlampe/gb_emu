/* cpu.h — the Sharp SM83 register file, flags, and step function (L2, L5-L10).
 *
 * This header is pure data plus inline helpers: it includes no emulation logic and
 * no SDL, so a unit test can instantiate a CPU on the stack and check flag
 * behaviour without a bus or a window.
 */
#ifndef GB_CPU_H
#define GB_CPU_H

#include "gbdefs.h"

typedef struct MMU MMU;

/* Flag bits inside F. Only the top four bits exist in silicon; the low nibble
 * always reads back as zero, so every write to F must mask with FLAG_MASK. */
enum {
    FLAG_Z = 1u << 7,   /* Zero: result was 0x00 */
    FLAG_N = 1u << 6,   /* Subtract: last ALU op was a subtraction */
    FLAG_H = 1u << 5,   /* Half-carry: carry out of bit 3 (or bit 11) */
    FLAG_C = 1u << 4    /* Carry: carry out of bit 7 (or bit 15) */
};
#define FLAG_MASK 0xF0u

typedef struct CPU {
    /* The register pairs are unions so that `cpu->hl` and `cpu->h`/`cpu->l`
     * are two names for the same storage and can never disagree.
     *
     * PORTABILITY: this layout assumes a little-endian host, which is true for
     * x86-64, ARM64 and RISC-V. On a big-endian host, F would land in the
     * high byte; the portable accessor form is shown in the course outline's
     * Appendix E.
     */
    union { struct { uint8_t f, a; }; uint16_t af; };
    union { struct { uint8_t c, b; }; uint16_t bc; };
    union { struct { uint8_t e, d; }; uint16_t de; };
    union { struct { uint8_t l, h; }; uint16_t hl; };

    uint16_t sp;         /* stack pointer: points at the last byte pushed */
    uint16_t pc;         /* program counter: address of the next opcode */

    bool ime;            /* interrupt master enable */
    bool ime_pending;    /* set by EI; IME turns on after the next instruction */
    bool halted;         /* HALT executed and no interrupt is pending */
    bool halt_bug;       /* HALT with IME=0 and IF&IE!=0: PC does not advance */
    bool stopped;        /* STOP executed; resumes on a joypad interrupt */

    uint64_t t_cycles;   /* total T-cycles executed since reset */

    /* Decode scratch, written by the handlers and consumed by cpu_step(). */
    uint8_t  opcode;        /* the opcode being executed */
    uint8_t  cb_opcode;     /* the second byte of a CB-prefixed instruction */
    uint32_t cb_cycles;     /* extra T-cycles contributed by the CB page */
    bool     branch_taken;   /* a conditional branch/return was taken */
} CPU;

/* --- Instruction table --------------------------------------------------
 * One entry per opcode. The table is the single source of truth for
 * instruction length, base cost, conditional cost, and the mnemonic that the
 * disassembler prints. Cycles live in data, not in code, so fixing timing is a
 * table edit rather than a rewrite.
 */
typedef struct {
    const char *mnemonic;      /* e.g. "LD A, d8" */
    uint8_t     len;           /* instruction length in bytes, 1-3 */
    uint8_t     cycles;        /* base T-cycles */
    uint8_t     cycles_taken;  /* T-cycles when a condition is taken, else 0 */
    void      (*exec)(CPU *c, MMU *m);
} Instr;

extern Instr OPS[256];       /* unprefixed page */
extern Instr OPS_CB[256];    /* CB-prefixed page */

/* --- Lifecycle and execution ------------------------------------------- */
void     cpu_reset(CPU *c, bool cgb);
uint32_t cpu_step(CPU *c, MMU *m);   /* execute one instruction, return T-cycles */

/* --- Flag helpers ------------------------------------------------------ */
static inline void cpu_set_flag(CPU *c, uint8_t flag, bool on)
{
    /* Clear then set, so the four flags stay independent and the low nibble
     * of F stays zero. */
    c->f = on ? (uint8_t)(c->f | flag) : (uint8_t)(c->f & ~flag);
}

static inline bool cpu_flag(const CPU *c, uint8_t flag)
{
    return (c->f & flag) != 0u;
}

/* --- Building the tables (called once from gb_init) ----------------------- */
void cpu_build_tables(void);

/* --- Services used by the interrupt logic and by tests --------------------- */
void     cpu_push16(CPU *c, MMU *m, uint16_t v);
uint16_t cpu_pop16(CPU *c, MMU *m);
bool     cpu_service_interrupt(CPU *c, MMU *m);  /* true if one was taken */
void     cpu_interrupt_vector(CPU *c, MMU *m, uint8_t bit);

/* --- Post-boot-ROM startup state (DMG) ---------------------------------
 * A boot ROM that runs on hardware would leave these values behind. Starting
 * here lets students bring up the CPU before the boot ROM is implemented.
 */
void cpu_reset_dmg(CPU *c);
void cpu_reset_cgb(CPU *c);

void cpu_serialize(CPU *c, FILE *f);
void cpu_deserialize(CPU *c, FILE *f);

#endif /* GB_CPU_H */
