#!/usr/bin/env python3
"""gen_opcode_tables.py — generate src/cpu_tables.inc from one table of facts.

The SM83's per-opcode facts (mnemonic, length, base cost, taken cost, handler)
are hand-written here exactly once, and the C tables are emitted with DESIGNATED
initializers. That matters: a positional initializer for an irregular 256-entry
table is one mis-counted row away from silently shifting every later opcode, which
is a bug that costs a whole lab session to find. Designated initializers make the
index part of the data, so the table cannot drift.

Usage: python3 tools/gen_opcode_tables.py   (writes src/cpu_tables.inc)
"""
import os
import sys

# Rows of eight: (mnemonic, length, cycles, cycles_if_taken, handler)
# None means "handled by the generated loops" (0x40-0x7F and 0x80-0xBF).
# An illegal opcode has cycles 4: hardware locks up, and four keeps the machine's
# clock consistent while cpu_step() reports the opcode.
ILL = ("illegal opcode", 1, 4, 0, "op_illegal")

TOP = [
    ("NOP", 1, 4, 0, "op_nop"),
    ("LD BC, d16", 3, 12, 0, "op_ld_r16_d16"),
    ("LD (BC), A", 1, 8, 0, "op_ld_mem_a"),
    ("INC BC", 1, 8, 0, "op_inc_r16"),
    ("INC B", 1, 4, 0, "op_inc_r8"),
    ("DEC B", 1, 4, 0, "op_dec_r8"),
    ("LD B, d8", 2, 8, 0, "op_ld_r8_d8"),
    ("RLCA", 1, 4, 0, "op_rlca"),

    ("LD (a16), SP", 3, 20, 0, "op_ld_a16_sp"),
    ("ADD HL, BC", 1, 8, 0, "op_add_hl_r16"),
    ("LD A, (BC)", 1, 8, 0, "op_ld_a_mem"),
    ("DEC BC", 1, 8, 0, "op_dec_r16"),
    ("INC C", 1, 4, 0, "op_inc_r8"),
    ("DEC C", 1, 4, 0, "op_dec_r8"),
    ("LD C, d8", 2, 8, 0, "op_ld_r8_d8"),
    ("RRCA", 1, 4, 0, "op_rrca"),

    ("STOP", 1, 4, 0, "op_stop"),
    ("LD DE, d16", 3, 12, 0, "op_ld_r16_d16"),
    ("LD (DE), A", 1, 8, 0, "op_ld_mem_a"),
    ("INC DE", 1, 8, 0, "op_inc_r16"),
    ("INC D", 1, 4, 0, "op_inc_r8"),
    ("DEC D", 1, 4, 0, "op_dec_r8"),
    ("LD D, d8", 2, 8, 0, "op_ld_r8_d8"),
    ("RLA", 1, 4, 0, "op_rla"),

    ("JR r8", 2, 12, 0, "op_jr_e8"),
    ("ADD HL, DE", 1, 8, 0, "op_add_hl_r16"),
    ("LD A, (DE)", 1, 8, 0, "op_ld_a_mem"),
    ("DEC DE", 1, 8, 0, "op_dec_r16"),
    ("INC E", 1, 4, 0, "op_inc_r8"),
    ("DEC E", 1, 4, 0, "op_dec_r8"),
    ("LD E, d8", 2, 8, 0, "op_ld_r8_d8"),
    ("RRA", 1, 4, 0, "op_rra"),

    ("JR NZ, r8", 2, 12, 8, "op_jr_cc"),
    ("LD HL, d16", 3, 12, 0, "op_ld_r16_d16"),
    ("LD (HL+), A", 1, 8, 0, "op_ld_mem_a"),
    ("INC HL", 1, 8, 0, "op_inc_r16"),
    ("INC H", 1, 4, 0, "op_inc_r8"),
    ("DEC H", 1, 4, 0, "op_dec_r8"),
    ("LD H, d8", 2, 8, 0, "op_ld_r8_d8"),
    ("DAA", 1, 4, 0, "op_daa"),

    ("JR Z, r8", 2, 12, 8, "op_jr_cc"),
    ("ADD HL, HL", 1, 8, 0, "op_add_hl_r16"),
    ("LD A, (HL+)", 1, 8, 0, "op_ld_a_mem"),
    ("DEC HL", 1, 8, 0, "op_dec_r16"),
    ("INC L", 1, 4, 0, "op_inc_r8"),
    ("DEC L", 1, 4, 0, "op_dec_r8"),
    ("LD L, d8", 2, 8, 0, "op_ld_r8_d8"),
    ("CPL", 1, 4, 0, "op_cpl"),

    ("JR NC, r8", 2, 12, 8, "op_jr_cc"),
    ("LD SP, d16", 3, 12, 0, "op_ld_r16_d16"),
    ("LD (HL-), A", 1, 8, 0, "op_ld_mem_a"),
    ("INC SP", 1, 8, 0, "op_inc_r16"),
    ("INC (HL)", 1, 12, 0, "op_inc_r8"),
    ("DEC (HL)", 1, 12, 0, "op_dec_r8"),
    ("LD (HL), d8", 2, 12, 0, "op_ld_r8_d8"),
    ("SCF", 1, 4, 0, "op_scf"),

    ("JR C, r8", 2, 12, 8, "op_jr_cc"),
    ("ADD HL, SP", 1, 8, 0, "op_add_hl_r16"),
    ("LD A, (HL-)", 1, 8, 0, "op_ld_a_mem"),
    ("DEC SP", 1, 8, 0, "op_dec_r16"),
    ("INC A", 1, 4, 0, "op_inc_r8"),
    ("DEC A", 1, 4, 0, "op_dec_r8"),
    ("LD A, d8", 2, 8, 0, "op_ld_r8_d8"),
    ("CCF", 1, 4, 0, "op_ccf"),
] + [None] * 128 + [
    ("RET NZ", 1, 20, 8, "op_ret_cc"),
    ("POP BC", 1, 12, 0, "op_pop"),
    ("JP NZ, a16", 3, 16, 12, "op_jp_cc"),
    ("JP a16", 3, 16, 0, "op_jp_a16"),
    ("CALL NZ, a16", 3, 24, 12, "op_call_cc"),
    ("PUSH BC", 1, 16, 0, "op_push"),
    ("ADD A, d8", 2, 8, 0, "op_alu_d8"),
    ("RST 00H", 1, 16, 0, "op_rst"),

    ("RET Z", 1, 20, 8, "op_ret_cc"),
    ("RET", 1, 16, 0, "op_ret"),
    ("JP Z, a16", 3, 16, 12, "op_jp_cc"),
    ("PREFIX CB", 1, 4, 0, "op_cb_prefix"),
    ("CALL Z, a16", 3, 24, 12, "op_call_cc"),
    ("CALL a16", 3, 24, 0, "op_call_a16"),
    ("ADC A, d8", 2, 8, 0, "op_alu_d8"),
    ("RST 08H", 1, 16, 0, "op_rst"),

    ("RET NC", 1, 20, 8, "op_ret_cc"),
    ("POP DE", 1, 12, 0, "op_pop"),
    ("JP NC, a16", 3, 16, 12, "op_jp_cc"),
    ILL,
    ("CALL NC, a16", 3, 24, 12, "op_call_cc"),
    ("PUSH DE", 1, 16, 0, "op_push"),
    ("SUB d8", 2, 8, 0, "op_alu_d8"),
    ("RST 10H", 1, 16, 0, "op_rst"),

    ("RET C", 1, 20, 8, "op_ret_cc"),
    ("RETI", 1, 16, 0, "op_reti"),
    ("JP C, a16", 3, 16, 12, "op_jp_cc"),
    ILL,
    ("CALL C, a16", 3, 24, 12, "op_call_cc"),
    ILL,
    ("SBC A, d8", 2, 8, 0, "op_alu_d8"),
    ("RST 18H", 1, 16, 0, "op_rst"),

    ("LDH (a8), A", 2, 12, 0, "op_ldh_a8_a"),
    ("POP HL", 1, 12, 0, "op_pop"),
    ("LD (C), A", 2, 8, 0, "op_ld_c_a"),
    ILL,
    ILL,
    ("PUSH HL", 1, 16, 0, "op_push"),
    ("AND d8", 2, 8, 0, "op_alu_d8"),
    ("RST 20H", 1, 16, 0, "op_rst"),

    ("ADD SP, r8", 2, 16, 0, "op_add_sp_e8"),
    ("JP HL", 1, 4, 0, "op_jp_hl"),
    ("LD (a16), A", 3, 16, 0, "op_ld_a16_a"),
    ILL,
    ILL,
    ILL,
    ("XOR d8", 2, 8, 0, "op_alu_d8"),
    ("RST 28H", 1, 16, 0, "op_rst"),

    ("LDH A, (a8)", 2, 12, 0, "op_ldh_a_a8"),
    ("POP AF", 1, 12, 0, "op_pop"),
    ("LD A, (C)", 2, 8, 0, "op_ld_a_c"),
    ("DI", 1, 4, 0, "op_di"),
    ILL,
    ("PUSH AF", 1, 16, 0, "op_push"),
    ("OR d8", 2, 8, 0, "op_alu_d8"),
    ("RST 30H", 1, 16, 0, "op_rst"),

    ("LD HL, SP+r8", 2, 12, 0, "op_ld_hl_sp_e8"),
    ("LD SP, HL", 1, 8, 0, "op_ld_sp_hl"),
    ("LD A, (a16)", 3, 16, 0, "op_ld_a_a16"),
    ("EI", 1, 4, 0, "op_ei"),
    ILL,
    ILL,
    ("CP d8", 2, 8, 0, "op_alu_d8"),
    ("RST 38H", 1, 16, 0, "op_rst"),
]

assert len(TOP) == 256, len(TOP)


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "src", "cpu_tables.inc")

    with open(path, "w") as f:
        f.write("/* cpu_tables.inc — GENERATED by tools/gen_opcode_tables.py.\n"
                  " *\n"
                  " * Do not edit by hand: run the generator instead. Every entry is\n"
                  " * designated, so an opcode can never be shifted into the wrong slot.\n"
                  " */\n\n")

        f.write("static const char *const MN_IRREGULAR[256] = {\n")
        for op, e in enumerate(TOP):
            if e is not None:
                f.write(f"    [0x{op:02X}] = {c_string(e[0])},\n")
        f.write("};\n\n")

        f.write("/* Instruction length in bytes, 1-3. */\n")
        f.write("static const uint8_t LEN_TOP[256] = {\n")
        for op, e in enumerate(TOP):
            if e is not None:
                f.write(f"    [0x{op:02X}] = {e[1]},\n")
        f.write("};\n\n")

        f.write("/* Base T-cycles. */\n")
        f.write("static const uint8_t CYC_TOP[256] = {\n")
        for op, e in enumerate(TOP):
            if e is not None:
                f.write(f"    [0x{op:02X}] = {e[2]},\n")
        f.write("};\n\n")

        f.write("/* Extra T-cycles when a conditional jump, call or return is taken. */\n")
        f.write("static const uint8_t CYC_TAKEN_TOP[256] = {\n")
        for op, e in enumerate(TOP):
            if e is not None and e[3] != 0:
                f.write(f"    [0x{op:02X}] = {e[3]},\n")
        f.write("};\n\n")

        f.write("/* Handlers; 0x40-0x7F and 0x80-0xBF are generated in cpu.c. */\n")
        f.write("static void (*const EXEC_TOP[256])(CPU *, MMU *) = {\n")
        for op, e in enumerate(TOP):
            if e is not None:
                f.write(f"    [0x{op:02X}] = {e[4]},\n")
        f.write("};\n")

    print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
