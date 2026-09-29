/* cpu.c — the Sharp SM83 core: decode tables, ALU, control flow, interrupts.
 *
 * Lesson mapping: L5 (decode/execute), L6 (8-bit ALU), L7 (16-bit/stack/flow),
 * L8 (CB page), L9 (timers are in timer.c), L10 (interrupts and HALT).
 *
 * Design rules that the rest of the project depends on:
 *   1. Timing is data: every instruction's length, base cost and taken cost live
 *      in the OPS[] table, so fixing timing is a table edit, not a rewrite.
 *   2. cpu_step() is the only function that advances the machine, and it never
 *      reads a clock, a random number or the host: emulated time comes from the
 *      instruction table alone. That is what makes replays deterministic.
 *   3. Interrupts are serviced BETWEEN instructions, never inside a handler.
 *
 * Reference implementation for CS 4XX "System Emulation".
 */
#include "cpu.h"

#include "mmu.h"

#include <string.h>

/* --- The instruction tables ---------------------------------------------
 * OPS[] and OPS_CB[] are declared in cpu.h and filled in by
 * cpu_build_tables(), which gb_init() calls once. The mnemonics are shared
 * with the debugger's disassembler so the two can never disagree.
 */
Instr OPS[256];
Instr OPS_CB[256];

/* ======================================================================
 * 1. Register file access
 * ====================================================================== */

/* read_r8 / write_r8 — the eight 8-bit registers by index.
 * Index 6 is (HL): a MEMORY access, not a register. That is why these two
 * helpers take the MMU: they may touch the bus, and so they can be a source of
 * surprising cycle counts and DMA conflicts later (L28).
 */
static uint8_t read_r8(CPU *c, MMU *m, unsigned i)
{
    switch (i) {
    case 0: return c->b;
    case 1: return c->c;
    case 2: return c->d;
    case 3: return c->e;
    case 4: return c->h;
    case 5: return c->l;
    case 6: return mmu_read8(m, c->hl);
    default: return c->a;
    }
}

static void write_r8(CPU *c, MMU *m, unsigned i, uint8_t v)
{
    switch (i) {
    case 0: c->b = v; break;
    case 1: c->c = v; break;
    case 2: c->d = v; break;
    case 3: c->e = v; break;
    case 4: c->h = v; break;
    case 5: c->l = v; break;
    case 6: mmu_write8(m, c->hl, v); break;
    default: c->a = v; break;
    }
}

/* read_r16 / write_r16 — the 16-bit register groups used by arithmetic.
 * Index 3 is SP, NOT AF: this is the "r16" set from the manual.
 */
static uint16_t read_r16(CPU *c, unsigned i)
{
    switch (i) {
    case 0: return c->bc;
    case 1: return c->de;
    case 2: return c->hl;
    default: return c->sp;
    }
}

static void write_r16(CPU *c, unsigned i, uint16_t v)
{
    switch (i) {
    case 0: c->bc = v; break;
    case 1: c->de = v; break;
    case 2: c->hl = v; break;
    default: c->sp = v; break;
    }
}

/* read_r16_stk / write_r16_stk — the PUSH/POP set. Index 3 is AF here, which
 * is how PUSH AF / POP AF differ from PUSH SP. POP AF must mask the low
 * nibble of F because those bits do not exist in silicon.
 */
static uint16_t read_r16_stk(CPU *c, unsigned i)
{
    switch (i) {
    case 0: return c->bc;
    case 1: return c->de;
    case 2: return c->hl;
    default: return c->af;
    }
}

static void write_r16_stk(CPU *c, unsigned i, uint16_t v)
{
    switch (i) {
    case 0: c->bc = v; break;
    case 1: c->de = v; break;
    case 2: c->hl = v; break;
    default: c->af = (uint16_t)(v & 0xFFF0u); break;   /* F low nibble is 0 */
    }
}

/* check_cond — the four condition codes: NZ, Z, NC, C. */
static bool check_cond(CPU *c, unsigned i)
{
    switch (i) {
    case 0: return !cpu_flag(c, FLAG_Z);
    case 1: return cpu_flag(c, FLAG_Z);
    case 2: return !cpu_flag(c, FLAG_C);
    default: return cpu_flag(c, FLAG_C);
    }
}

static uint8_t fetch8(CPU *c, MMU *m)
{
    return mmu_read8(m, c->pc++);
}

static uint16_t fetch16(CPU *c, MMU *m)
{
    uint16_t lo = fetch8(c, m);
    uint16_t hi = fetch8(c, m);
    return (uint16_t)(lo | (hi << 8));
}

/* ======================================================================
 * 2. The ALU
 *
 * Every flag rule in the SM83 is written out exactly once, here. Blargg's
 * cpu_instrs fails on the first approximated half-carry, so there is no
 * "close enough": the half-carry is computed from the low nibble of each
 * operand, never guessed from the result.
 * ====================================================================== */

/* alu_add — ADD A,x and (with carry_in) ADC A,x. */
static uint8_t alu_add(CPU *c, uint8_t lhs, uint8_t rhs, bool carry_in)
{
    unsigned carry = (carry_in && cpu_flag(c, FLAG_C)) ? 1u : 0u;
    unsigned result = (unsigned)lhs + (unsigned)rhs + carry;

    cpu_set_flag(c, FLAG_Z, (result & 0xFFu) == 0u);
    cpu_set_flag(c, FLAG_N, false);
    /* Half-carry: did the low nibble carry into bit 4? */
    cpu_set_flag(c, FLAG_H, ((lhs & 0x0Fu) + (rhs & 0x0Fu) + carry) > 0x0Fu);
    /* Full carry: did the byte carry into bit 8? */
    cpu_set_flag(c, FLAG_C, result > 0xFFu);
    return (uint8_t)result;
}

/* alu_sub — SUB and (with carry_in) SBC. `store` is false for CP, which
 * computes the flags but discards the result. */
static uint8_t alu_sub(CPU *c, uint8_t lhs, uint8_t rhs, bool carry_in, bool store)
{
    unsigned carry = (carry_in && cpu_flag(c, FLAG_C)) ? 1u : 0u;
    unsigned result = (unsigned)lhs - (unsigned)rhs - carry;

    cpu_set_flag(c, FLAG_Z, (result & 0xFFu) == 0u);
    cpu_set_flag(c, FLAG_N, true);
    /* Half-borrow: did the low nibble borrow? Compare as unsigned bytes. */
    cpu_set_flag(c, FLAG_H, ((lhs & 0x0Fu) - (rhs & 0x0Fu) - carry) & 0x10u);
    /* Full borrow: did the byte borrow? */
    cpu_set_flag(c, FLAG_C, ((unsigned)lhs - (unsigned)rhs - carry) & 0x100u);
    return store ? (uint8_t)result : lhs;
}

/* alu_and / alu_or / alu_xor — the bitwise group. All three clear N and C;
 * AND sets H, OR and XOR clear it. */
static uint8_t alu_and(CPU *c, uint8_t v)
{
    c->a &= v;
    cpu_set_flag(c, FLAG_Z, c->a == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, true);
    cpu_set_flag(c, FLAG_C, false);
    return c->a;
}

static uint8_t alu_xor(CPU *c, uint8_t v)
{
    c->a ^= v;
    cpu_set_flag(c, FLAG_Z, c->a == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, false);
    return c->a;
}

static uint8_t alu_or(CPU *c, uint8_t v)
{
    c->a |= v;
    cpu_set_flag(c, FLAG_Z, c->a == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, false);
    return c->a;
}

/* alu_inc / alu_dec — INC and DEC. Both leave C alone, which is the single
 * most common flag bug in student emulators. */
static uint8_t alu_inc(CPU *c, uint8_t v)
{
    uint8_t r = (uint8_t)(v + 1u);
    cpu_set_flag(c, FLAG_Z, r == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, (v & 0x0Fu) == 0x0Fu);
    return r;   /* C is deliberately untouched. */
}

static uint8_t alu_dec(CPU *c, uint8_t v)
{
    uint8_t r = (uint8_t)(v - 1u);
    cpu_set_flag(c, FLAG_Z, r == 0u);
    cpu_set_flag(c, FLAG_N, true);
    cpu_set_flag(c, FLAG_H, (v & 0x0Fu) == 0x00u);
    return r;   /* C is deliberately untouched. */
}

/* alu_add16 — ADD HL,r16 and ADD SP,e8's high part. H and C come from bit 11
 * and bit 15 respectively, not bit 3 and bit 7. */
static uint16_t alu_add16(CPU *c, uint16_t lhs, uint16_t rhs)
{
    unsigned result = (unsigned)lhs + (unsigned)rhs;

    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, ((lhs & 0x0FFFu) + (rhs & 0x0FFFu)) > 0x0FFFu);
    cpu_set_flag(c, FLAG_C, result > 0xFFFFu);
    /* Z is NOT affected by 16-bit ADD. */
    return (uint16_t)result;
}

/* alu_daa — decimal adjust, used after ADD/ADC or SUB/SBC on packed BCD.
 * The previous operation's N, H and C flags drive the correction; N must
 * survive so that DAA after a subtraction takes the subtract branch. */
static void alu_daa(CPU *c)
{
    uint8_t correction = 0;
    bool carry = cpu_flag(c, FLAG_C);

    if (!cpu_flag(c, FLAG_N)) {
        if (cpu_flag(c, FLAG_H) || (c->a & 0x0Fu) > 0x09u) correction |= 0x06u;
        if (carry || c->a > 0x99u) { correction |= 0x60u; carry = true; }
    } else {
        if (cpu_flag(c, FLAG_H)) correction |= 0x06u;
        if (carry) correction |= 0x60u;
    }

    c->a = (uint8_t)(c->a + (cpu_flag(c, FLAG_N) ? -correction : correction));
    cpu_set_flag(c, FLAG_Z, c->a == 0u);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry);
}

/* alu_rlc / alu_rrc / alu_rl / alu_rr — the rotates.
 * Z comes from the result, C from the bit rotated out, N and H are cleared. */
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

static uint8_t alu_rrc(CPU *c, uint8_t v)
{
    uint8_t carry = (uint8_t)(v & 1u);
    v = (uint8_t)((v >> 1) | (uint8_t)(carry << 7));
    cpu_set_flag(c, FLAG_Z, v == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry != 0u);
    return v;
}

static uint8_t alu_rl(CPU *c, uint8_t v)
{
    uint8_t carry = (uint8_t)(v >> 7);
    v = (uint8_t)((v << 1) | (cpu_flag(c, FLAG_C) ? 1u : 0u));
    cpu_set_flag(c, FLAG_Z, v == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry != 0u);
    return v;
}

static uint8_t alu_rr(CPU *c, uint8_t v)
{
    uint8_t carry = (uint8_t)(v & 1u);
    v = (uint8_t)((v >> 1) | (cpu_flag(c, FLAG_C) ? 0x80u : 0u));
    cpu_set_flag(c, FLAG_Z, v == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry != 0u);
    return v;
}

static uint8_t alu_sla(CPU *c, uint8_t v)
{
    uint8_t carry = (uint8_t)(v >> 7);
    v = (uint8_t)(v << 1);
    cpu_set_flag(c, FLAG_Z, v == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry != 0u);
    return v;
}

/* alu_sra — arithmetic shift right: bit 7 keeps its old value. */
static uint8_t alu_sra(CPU *c, uint8_t v)
{
    uint8_t carry = (uint8_t)(v & 1u);
    v = (uint8_t)((v >> 1) | (v & 0x80u));
    cpu_set_flag(c, FLAG_Z, v == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry != 0u);
    return v;
}

static uint8_t alu_swap(CPU *c, uint8_t v)
{
    v = (uint8_t)((v << 4) | (v >> 4));
    cpu_set_flag(c, FLAG_Z, v == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, false);
    return v;
}

static uint8_t alu_srl(CPU *c, uint8_t v)
{
    uint8_t carry = (uint8_t)(v & 1u);
    v = (uint8_t)(v >> 1);
    cpu_set_flag(c, FLAG_Z, v == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry != 0u);
    return v;
}

/* ======================================================================
 * 3. Handlers — the unprefixed page
 *
 * Handlers read `c->opcode` when they cover a family of opcodes, so one
 * function implements eight opcodes. That is why the table can stay small
 * enough to read in one sitting.
 * ====================================================================== */

static void op_nop(CPU *c, MMU *m) { (void)c; (void)m; }

static void op_illegal(CPU *c, MMU *m)
{
    (void)m;
    /* Real hardware locks up on an illegal opcode. Stopping the CPU and
     * reporting the address is a debugging aid, not a hardware model. */
    gb_log("illegal opcode %02X at %04X", c->opcode, (uint16_t)(c->pc - 1));
    c->halted = true;
}

static void op_ld_r16_d16(CPU *c, MMU *m)
{
    write_r16(c, (c->opcode >> 4) & 3u, fetch16(c, m));
}

static void op_ld_r8_d8(CPU *c, MMU *m)
{
    write_r8(c, m, (c->opcode >> 3) & 7u, fetch8(c, m));
}

static void op_ld_r8_r8(CPU *c, MMU *m)
{
    unsigned dst = (c->opcode >> 3) & 7u;
    unsigned src = c->opcode & 7u;
    /* Read before writing: when dst is (HL) the write must not change the
     * address that the read used. */
    uint8_t v = read_r8(c, m, src);
    write_r8(c, m, dst, v);
}

/* op_alu_r8 / op_alu_d8 — one handler per operand kind. */
static void op_alu_r8(CPU *c, MMU *m)
{
    unsigned kind = (c->opcode >> 3) & 7u;
    uint8_t v = read_r8(c, m, c->opcode & 7u);

    switch (kind) {
    case 0: c->a = alu_add(c, c->a, v, false); break;
    case 1: c->a = alu_add(c, c->a, v, true); break;
    case 2: c->a = alu_sub(c, c->a, v, false, true); break;
    case 3: c->a = alu_sub(c, c->a, v, true, true); break;
    case 4: c->a = alu_and(c, v); break;
    case 5: c->a = alu_xor(c, v); break;
    case 6: c->a = alu_or(c, v); break;
    default: (void)alu_sub(c, c->a, v, false, false); break;  /* CP */
    }
}

static void op_alu_d8(CPU *c, MMU *m)
{
    unsigned kind = (c->opcode >> 3) & 7u;
    uint8_t v = fetch8(c, m);

    switch (kind) {
    case 0: c->a = alu_add(c, c->a, v, false); break;
    case 1: c->a = alu_add(c, c->a, v, true); break;
    case 2: c->a = alu_sub(c, c->a, v, false, true); break;
    case 3: c->a = alu_sub(c, c->a, v, true, true); break;
    case 4: c->a = alu_and(c, v); break;
    case 5: c->a = alu_xor(c, v); break;
    case 6: c->a = alu_or(c, v); break;
    default: (void)alu_sub(c, c->a, v, false, false); break;  /* CP */
    }
}

static void op_inc_r8(CPU *c, MMU *m)
{
    unsigned i = (c->opcode >> 3) & 7u;
    write_r8(c, m, i, alu_inc(c, read_r8(c, m, i)));
}

static void op_dec_r8(CPU *c, MMU *m)
{
    unsigned i = (c->opcode >> 3) & 7u;
    write_r8(c, m, i, alu_dec(c, read_r8(c, m, i)));
}

static void op_inc_r16(CPU *c, MMU *m)
{
    (void)m;
    unsigned i = (c->opcode >> 4) & 3u;
    write_r16(c, i, (uint16_t)(read_r16(c, i) + 1u));
}

static void op_dec_r16(CPU *c, MMU *m)
{
    (void)m;
    unsigned i = (c->opcode >> 4) & 3u;
    write_r16(c, i, (uint16_t)(read_r16(c, i) - 1u));
}

/* LD (BC),A / LD (DE),A / LD (HL+),A / LD (HL-),A */
static void op_ld_mem_a(CPU *c, MMU *m)
{
    uint16_t addr = (c->opcode == 0x02) ? c->bc :
                    (c->opcode == 0x12) ? c->de : c->hl;
    mmu_write8(m, addr, c->a);
    if (c->opcode == 0x22) c->hl++;
    if (c->opcode == 0x32) c->hl--;
}

/* LD A,(BC) / LD A,(DE) / LD A,(HL+) / LD A,(HL-) */
static void op_ld_a_mem(CPU *c, MMU *m)
{
    uint16_t addr = (c->opcode == 0x0A) ? c->bc :
                    (c->opcode == 0x1A) ? c->de : c->hl;
    c->a = mmu_read8(m, addr);
    if (c->opcode == 0x2A) c->hl++;
    if (c->opcode == 0x3A) c->hl--;
}

static void op_ld_a16_sp(CPU *c, MMU *m)
{
    uint16_t addr = fetch16(c, m);
    mmu_write16(m, addr, c->sp);
}

static void op_add_hl_r16(CPU *c, MMU *m)
{
    (void)m;
    c->hl = alu_add16(c, c->hl, read_r16(c, (c->opcode >> 4) & 3u));
}

static void op_jp_a16(CPU *c, MMU *m) { c->pc = fetch16(c, m); }
static void op_jp_hl(CPU *c, MMU *m) { (void)m; c->pc = c->hl; }

static void op_jp_cc(CPU *c, MMU *m)
{
    uint16_t addr = fetch16(c, m);
    if (check_cond(c, (c->opcode >> 3) & 3u)) {
        c->pc = addr;
        c->branch_taken = true;
    }
}

static void op_jr_e8(CPU *c, MMU *m)
{
    /* JR uses a SIGNED 8-bit displacement relative to the byte after the
     * displacement, which is where PC already points. Casting through int8_t is
     * the portable way to say that. */
    int8_t off = (int8_t)fetch8(c, m);
    c->pc = (uint16_t)(c->pc + off);
}

static void op_jr_cc(CPU *c, MMU *m)
{
    int8_t off = (int8_t)fetch8(c, m);
    if (check_cond(c, (c->opcode >> 3) & 3u)) {
        c->pc = (uint16_t)(c->pc + off);
        c->branch_taken = true;
    }
}

static void op_call_a16(CPU *c, MMU *m)
{
    uint16_t addr = fetch16(c, m);
    cpu_push16(c, m, c->pc);
    c->pc = addr;
}

static void op_call_cc(CPU *c, MMU *m)
{
    uint16_t addr = fetch16(c, m);
    if (check_cond(c, (c->opcode >> 3) & 3u)) {
        cpu_push16(c, m, c->pc);
        c->pc = addr;
        c->branch_taken = true;
    }
}

static void op_ret(CPU *c, MMU *m) { c->pc = cpu_pop16(c, m); }

static void op_ret_cc(CPU *c, MMU *m)
{
    /* The condition is checked BEFORE the return address is popped: a
     * not-taken RET cc must not disturb SP. */
    if (check_cond(c, (c->opcode >> 3) & 3u)) {
        c->pc = cpu_pop16(c, m);
        c->branch_taken = true;
    }
}

static void op_reti(CPU *c, MMU *m)
{
    c->pc = cpu_pop16(c, m);
    c->ime = true;   /* RETI re-enables interrupts immediately */
}

static void op_push(CPU *c, MMU *m)
{
    unsigned i = (c->opcode >> 4) & 3u;
    cpu_push16(c, m, read_r16_stk(c, i));
}

static void op_pop(CPU *c, MMU *m)
{
    unsigned i = (c->opcode >> 4) & 3u;
    write_r16_stk(c, i, cpu_pop16(c, m));
}

static void op_rst(CPU *c, MMU *m)
{
    cpu_push16(c, m, c->pc);
    c->pc = (uint16_t)(c->opcode & 0x38u);
}

static void op_ldh_a8_a(CPU *c, MMU *m)
{
    mmu_write8(m, (uint16_t)(0xFF00u | fetch8(c, m)), c->a);
}

static void op_ldh_a_a8(CPU *c, MMU *m)
{
    c->a = mmu_read8(m, (uint16_t)(0xFF00u | fetch8(c, m)));
}

static void op_ld_c_a(CPU *c, MMU *m) { mmu_write8(m, (uint16_t)(0xFF00u | c->c), c->a); }
static void op_ld_a_c(CPU *c, MMU *m) { c->a = mmu_read8(m, (uint16_t)(0xFF00u | c->c)); }

static void op_ld_a16_a(CPU *c, MMU *m) { mmu_write8(m, fetch16(c, m), c->a); }
static void op_ld_a_a16(CPU *c, MMU *m) { c->a = mmu_read8(m, fetch16(c, m)); }

static void op_ld_sp_hl(CPU *c, MMU *m) { (void)m; c->sp = c->hl; }

static void op_ld_hl_sp_e8(CPU *c, MMU *m)
{
    /* LD HL,SP+r8 sets flags from the low byte, exactly like ADD SP,r8. */
    int8_t off = (int8_t)fetch8(c, m);
    unsigned r = (unsigned)(c->sp & 0xFFu) + (unsigned)(uint8_t)off;

    cpu_set_flag(c, FLAG_Z, false);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, ((c->sp & 0x0Fu) + ((uint8_t)off & 0x0Fu)) > 0x0Fu);
    cpu_set_flag(c, FLAG_C, r > 0xFFu);

    c->hl = (uint16_t)(c->sp + off);
}

static void op_add_sp_e8(CPU *c, MMU *m)
{
    int8_t off = (int8_t)fetch8(c, m);
    unsigned r = (unsigned)(c->sp & 0xFFu) + (unsigned)(uint8_t)off;

    cpu_set_flag(c, FLAG_Z, false);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, ((c->sp & 0x0Fu) + ((uint8_t)off & 0x0Fu)) > 0x0Fu);
    cpu_set_flag(c, FLAG_C, r > 0xFFu);

    c->sp = (uint16_t)(c->sp + off);
}

static void op_rlca(CPU *c, MMU *m)
{
    (void)m;
    uint8_t carry = (uint8_t)(c->a >> 7);
    c->a = (uint8_t)((c->a << 1) | carry);
    /* The fast rotates clear Z unconditionally, unlike the CB versions. */
    cpu_set_flag(c, FLAG_Z, false);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry != 0u);
}

static void op_rrca(CPU *c, MMU *m)
{
    (void)m;
    uint8_t carry = (uint8_t)(c->a & 1u);
    c->a = (uint8_t)((c->a >> 1) | (uint8_t)(carry << 7));
    cpu_set_flag(c, FLAG_Z, false);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry != 0u);
}

static void op_rla(CPU *c, MMU *m)
{
    (void)m;
    uint8_t carry = (uint8_t)(c->a >> 7);
    c->a = (uint8_t)((c->a << 1) | (cpu_flag(c, FLAG_C) ? 1u : 0u));
    cpu_set_flag(c, FLAG_Z, false);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry != 0u);
}

static void op_rra(CPU *c, MMU *m)
{
    (void)m;
    uint8_t carry = (uint8_t)(c->a & 1u);
    c->a = (uint8_t)((c->a >> 1) | (cpu_flag(c, FLAG_C) ? 0x80u : 0u));
    cpu_set_flag(c, FLAG_Z, false);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, carry != 0u);
}

static void op_daa(CPU *c, MMU *m) { (void)m; alu_daa(c); }
static void op_cpl(CPU *c, MMU *m) { (void)m; c->a = (uint8_t)~c->a; cpu_set_flag(c, FLAG_N, true); cpu_set_flag(c, FLAG_H, true); }

static void op_scf(CPU *c, MMU *m)
{
    (void)m;
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, true);
}

static void op_ccf(CPU *c, MMU *m)
{
    (void)m;
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, false);
    cpu_set_flag(c, FLAG_C, !cpu_flag(c, FLAG_C));
}

/* op_halt — HALT. With IME set, the CPU stops until an interrupt is pending.
 * With IME clear and an interrupt already pending, hardware does not halt: it
 * fails to advance PC for one instruction, the "HALT bug", which several games
 * and Mooneye's halt_bug test detect. */
static void op_halt(CPU *c, MMU *m)
{
    if (c->ime) {
        c->halted = true;
    } else if (m->if_reg & m->ie & 0x1Fu) {
        c->halt_bug = true;
    } else {
        c->halted = true;
    }
}

/* op_stop — STOP. On CGB, STOP performs a pending speed switch (KEY1 bit 0). */
static void op_stop(CPU *c, MMU *m)
{
    if (m->cgb && (m->io[0x4D] & 1u)) {
        m->double_speed = !m->double_speed;
        m->io[0x4D] = (uint8_t)((m->double_speed ? 0x80u : 0x00u));
    }
    c->stopped = true;
}

static void op_di(CPU *c, MMU *m) { (void)m; c->ime = false; c->ime_pending = false; }
static void op_ei(CPU *c, MMU *m) { (void)m; c->ime_pending = true; }

static void op_cb_prefix(CPU *c, MMU *m)
{
    c->cb_opcode = fetch8(c, m);
    const Instr *in = &OPS_CB[c->cb_opcode];
    c->cb_cycles = in->cycles;
    if (!in->exec) {
        gb_log("illegal CB opcode %02X at %04X", c->cb_opcode, (uint16_t)(c->pc - 2));
        c->halted = true;
        return;
    }
    in->exec(c, m);
}

/* ======================================================================
 * 4. Handlers — the CB page
 * ====================================================================== */

/* op_cb_rotate — RLC/RRC/RL/RR/SLA/SRA/SWAP/SRL on any r8. */
static void op_cb_rotate(CPU *c, MMU *m)
{
    unsigned kind = (c->cb_opcode >> 3) & 7u;
    unsigned i = c->cb_opcode & 7u;
    uint8_t v = read_r8(c, m, i);

    switch (kind) {
    case 0: v = alu_rlc(c, v); break;
    case 1: v = alu_rrc(c, v); break;
    case 2: v = alu_rl(c, v); break;
    case 3: v = alu_rr(c, v); break;
    case 4: v = alu_sla(c, v); break;
    case 5: v = alu_sra(c, v); break;
    case 6: v = alu_swap(c, v); break;
    default: v = alu_srl(c, v); break;
    }

    write_r8(c, m, i, v);
}

/* op_cb_bit — BIT b,r8. Sets Z to the INVERSE of the tested bit, sets H,
 * clears N, and deliberately leaves C untouched. */
static void op_cb_bit(CPU *c, MMU *m)
{
    unsigned bit = (c->cb_opcode >> 3) & 7u;
    unsigned i = c->cb_opcode & 7u;
    uint8_t v = read_r8(c, m, i);

    cpu_set_flag(c, FLAG_Z, ((v >> bit) & 1u) == 0u);
    cpu_set_flag(c, FLAG_N, false);
    cpu_set_flag(c, FLAG_H, true);
}

/* op_cb_res / op_cb_set — RES and SET touch no flags at all. */
static void op_cb_res(CPU *c, MMU *m)
{
    unsigned bit = (c->cb_opcode >> 3) & 7u;
    unsigned i = c->cb_opcode & 7u;
    write_r8(c, m, i, (uint8_t)(read_r8(c, m, i) & ~(1u << bit)));
}

static void op_cb_set(CPU *c, MMU *m)
{
    unsigned bit = (c->cb_opcode >> 3) & 7u;
    unsigned i = c->cb_opcode & 7u;
    write_r8(c, m, i, (uint8_t)(read_r8(c, m, i) | (1u << bit)));
}

/* ======================================================================
 * 5. Table construction
 *
 * The irregular opcodes are written out; the regular families (LD r8,r8',
 * ALU r8, and the whole CB page) are generated, because their mnemonics and
 * cycles follow a pattern. Generated entries are the reason a single typo cannot
 * silently give an opcode the wrong length.
 * ====================================================================== */

static const char *R8_NAME[8] = { "B", "C", "D", "E", "H", "L", "(HL)", "A" };
static const char *ALU_NAME[8] = { "ADD A", "ADC A", "SUB", "SBC A", "AND", "XOR", "OR", "CP" };
static const char *CB_NAME[8] = { "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SWAP", "SRL" };

/* The opcode tables are generated from one table of facts: see
 * tools/gen_opcode_tables.py. They are included rather than written by hand
 * because a positional initializer for a 256-entry table is one miscounted
 * row away from silently shifting every later opcode. */
#include "cpu_tables.inc"

/* CB page cycles: 8 for register operands, 16 when the operand is (HL).
 * BIT uses 12 instead of 16 because it does not write back. */
static uint8_t cb_cycles(unsigned op)
{
    unsigned kind = op >> 3;   /* 0-7 rotate, 8-15 BIT, 16-23 RES, 24-31 SET */
    unsigned reg = op & 7u;
    if (kind >= 8u && kind < 16u)
        return (reg == 6u) ? 12u : 8u;
    return (reg == 6u) ? 16u : 8u;
}

static char mn_gen[512][16];   /* storage for generated mnemonics */

void cpu_build_tables(void)
{
    static bool built = false;
    if (built) return;    /* gb_init() may be called more than once per process */
    built = true;

    for (unsigned op = 0; op < 256u; op++) {
        OPS[op].mnemonic = MN_IRREGULAR[op];
        OPS[op].len = LEN_TOP[op];
        OPS[op].cycles = CYC_TOP[op];
        OPS[op].cycles_taken = CYC_TAKEN_TOP[op];
        OPS[op].exec = EXEC_TOP[op];
    }

    /* LD r8, r8' (0x40-0x7F), with 0x76 being HALT. */
    for (unsigned op = 0x40u; op <= 0x7Fu; op++) {
        char *buf = mn_gen[op];
        if (op == 0x76u) {
            snprintf(buf, 16, "HALT");
            OPS[op].mnemonic = buf;
            OPS[op].exec = op_halt;
            continue;
        }
        unsigned dst = (op >> 3) & 7u;
        unsigned src = op & 7u;
        snprintf(buf, 16, "LD %s, %s", R8_NAME[dst], R8_NAME[src]);
        OPS[op].mnemonic = buf;
        OPS[op].len = 1;                 /* LD r8,r8 has no operand bytes */
        OPS[op].exec = op_ld_r8_r8;
        /* The (HL) forms cost 8 T-cycles instead of 4. */
        if (dst == 6u || src == 6u) OPS[op].cycles = 8;
    }

    /* ALU A, r8 (0x80-0xBF). */
    for (unsigned op = 0x80u; op <= 0xBFu; op++) {
        char *buf = mn_gen[op];
        unsigned kind = (op >> 3) & 7u;
        unsigned src = op & 7u;
        snprintf(buf, 16, "%s, %s", ALU_NAME[kind], R8_NAME[src]);
        OPS[op].mnemonic = buf;
        OPS[op].len = 1;                  /* ALU A,r8 has no operand bytes */
        OPS[op].exec = op_alu_r8;
        OPS[op].cycles = (src == 6u) ? 8u : 4u;
    }

    /* CB page: 0x00-0x3F rotates/shifts, 0x40-0x7F BIT, 0x80-0xBF RES,
     * 0xC0-0xFF SET. */
    for (unsigned op = 0; op < 256u; op++) {
        char *buf = mn_gen[256u + op];
        unsigned kind = op >> 3;
        unsigned reg = op & 7u;

        if (kind < 8u)
            snprintf(buf, 16, "%s %s", CB_NAME[kind], R8_NAME[reg]);
        else if (kind < 16u)
            snprintf(buf, 16, "BIT %u, %s", kind - 8u, R8_NAME[reg]);
        else if (kind < 24u)
            snprintf(buf, 16, "RES %u, %s", kind - 16u, R8_NAME[reg]);
        else
            snprintf(buf, 16, "SET %u, %s", kind - 24u, R8_NAME[reg]);

        OPS_CB[op].mnemonic = buf;
        OPS_CB[op].len = 2u;
        OPS_CB[op].cycles = cb_cycles(op);
        OPS_CB[op].cycles_taken = 0u;

        if (kind < 8u)       OPS_CB[op].exec = op_cb_rotate;
        else if (kind < 16u) OPS_CB[op].exec = op_cb_bit;
        else if (kind < 24u) OPS_CB[op].exec = op_cb_res;
        else                 OPS_CB[op].exec = op_cb_set;
    }
}

/* ======================================================================
 * 6. Stack, interrupts, and the step function
 * ====================================================================== */

void cpu_push16(CPU *c, MMU *m, uint16_t v)
{
    /* The stack grows DOWN and stores the HIGH byte first. The order matters:
     * games read their own stack, and OAM DMA conflicts in L28 depend on the
     * exact bus accesses, not just the final SP value. */
    c->sp--;
    mmu_write8(m, c->sp, (uint8_t)(v >> 8));
    c->sp--;
    mmu_write8(m, c->sp, (uint8_t)(v & 0xFFu));
}

uint16_t cpu_pop16(CPU *c, MMU *m)
{
    uint8_t lo = mmu_read8(m, c->sp++);
    uint8_t hi = mmu_read8(m, c->sp++);
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

/* cpu_service_interrupt — called once per instruction, before the fetch.
 *
 * Two behaviours live here and both are directly tested:
 *   1. HALT exits on any pending interrupt whose IE bit is set, even when
 *      IME is clear, and in that case no handler runs.
 *   2. The lowest set bit wins when several interrupts are pending.
 */
bool cpu_service_interrupt(CPU *c, MMU *m)
{
    uint8_t pending = (uint8_t)(m->if_reg & m->ie & 0x1Fu);
    if (pending == 0u)
        return false;

    c->halted = false;    /* an interrupt wakes a halted CPU */
    if (!c->ime)
        return false;     /* ...but without IME no handler is entered */

    for (unsigned bit = 0; bit < 5u; bit++) {
        if ((pending >> bit) & 1u) {
            cpu_interrupt_vector(c, m, (uint8_t)bit);
            return true;
        }
    }
    return false;
}

void cpu_interrupt_vector(CPU *c, MMU *m, uint8_t bit)
{
    c->ime = false;                     /* hardware clears IME */
    mmu_if_clear(m, (uint8_t)(1u << bit));
    cpu_push16(c, m, c->pc);            /* 16 T-cycles */
    c->pc = (uint16_t)(0x40u + 8u * bit);  /* vector table */
}

/* cpu_step — execute exactly one instruction and return its T-cycles.
 *
 * This is the machine's only source of emulated time. It never consults a wall
 * clock, never sleeps and never yields: that is what makes gb_run_frame()
 * deterministic, save states small and replays exact.
 */
uint32_t cpu_step(CPU *c, MMU *m)
{
    /* A halted CPU still consumes 4 T-cycles per cycle so that the rest of the
     * machine (timer, PPU, APU) keeps running. */
    if (c->halted) {
        if ((m->if_reg & m->ie & 0x1Fu) != 0u)
            c->halted = false;      /* fall through and service it below */
        else {
            c->t_cycles += 4u;
            return 4u;
        }
    }

    /* STOP waits for a joypad interrupt (CGB: or a speed-switch). */
    if (c->stopped) {
        if ((m->if_reg & IF_JOYPAD) != 0u)
            c->stopped = false;
        else {
            c->t_cycles += 4u;
            return 4u;
        }
    }

    /* Interrupts are serviced BETWEEN instructions, never inside a handler. */
    if (cpu_service_interrupt(c, m)) {
        c->t_cycles += 20u;             /* 5 M-cycles to push PC and jump */
        return 20u;
    }

    /* EI's effect is delayed by one instruction, so it takes effect HERE:
     * after the instruction that followed EI has completed, and before the next
     * one starts. Applying it at the end of the EI instruction itself would
     * service the interrupt one instruction too early. */
    if (c->ime_pending) {
        c->ime = true;
        c->ime_pending = false;
    }

    /* Fetch. The HALT bug means the opcode is read WITHOUT advancing PC, so
     * the following instruction's first byte is executed twice. */
    if (c->halt_bug) {
        c->opcode = mmu_read8(m, c->pc);
        c->halt_bug = false;
    } else {
        c->opcode = mmu_read8(m, c->pc++);
    }

    const Instr *in = &OPS[c->opcode];

    c->branch_taken = false;
    c->cb_cycles = 0u;

    if (!in->exec) {
        op_illegal(c, m);
    } else {
        in->exec(c, m);
    }

    uint32_t cycles = in->cycles;
    if (c->branch_taken)
        cycles += in->cycles_taken;
    cycles += c->cb_cycles;

    c->t_cycles += cycles;
    return cycles;
}

/* ======================================================================
 * 7. Reset and save states
 * ====================================================================== */

void cpu_reset_dmg(CPU *c)
{
    /* Post-boot-ROM DMG values, as documented in Pan Docs. A build that runs
     * a real boot ROM will instead observe these on the way out of it. */
    c->af = 0x01B0;   /* A=0x01, F=0xB0: Z=1, N=0, H=1, C=1 */
    c->bc = 0x0013;
    c->de = 0x00D8;
    c->hl = 0x014D;
    c->sp = 0xFFFE;
    c->pc = 0x0100;   /* cartridge entry point */
    c->ime = false;
    c->ime_pending = false;
    c->halted = false;
    c->halt_bug = false;
    c->stopped = false;
    c->t_cycles = 0;
    c->opcode = 0;
    c->cb_opcode = 0;
    c->cb_cycles = 0;
    c->branch_taken = false;
}

void cpu_reset_cgb(CPU *c)
{
    cpu_reset_dmg(c);
    c->af = 0x1180;   /* CGB post-boot values */
    c->bc = 0x0000;
    c->de = 0xFF56;
    c->hl = 0x000D;
}

void cpu_reset(CPU *c, bool cgb)
{
    if (cgb) cpu_reset_cgb(c);
    else     cpu_reset_dmg(c);
}

void cpu_serialize(CPU *c, FILE *f)
{
    GB_SER(c->af, f); GB_SER(c->bc, f); GB_SER(c->de, f); GB_SER(c->hl, f);
    GB_SER(c->sp, f); GB_SER(c->pc, f);
    GB_SER(c->ime, f); GB_SER(c->ime_pending, f);
    GB_SER(c->halted, f); GB_SER(c->halt_bug, f); GB_SER(c->stopped, f);
    GB_SER(c->t_cycles, f);
    GB_SER(c->opcode, f); GB_SER(c->cb_opcode, f);
    GB_SER(c->cb_cycles, f); GB_SER(c->branch_taken, f);
}

void cpu_deserialize(CPU *c, FILE *f)
{
    GB_DESER(c->af, f); GB_DESER(c->bc, f); GB_DESER(c->de, f); GB_DESER(c->hl, f);
    GB_DESER(c->sp, f); GB_DESER(c->pc, f);
    GB_DESER(c->ime, f); GB_DESER(c->ime_pending, f);
    GB_DESER(c->halted, f); GB_DESER(c->halt_bug, f); GB_DESER(c->stopped, f);
    GB_DESER(c->t_cycles, f);
    GB_DESER(c->opcode, f); GB_DESER(c->cb_opcode, f);
    GB_DESER(c->cb_cycles, f); GB_DESER(c->branch_taken, f);
}
