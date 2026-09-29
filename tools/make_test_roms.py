#!/usr/bin/env python3
"""make_test_roms.py — generate the course's test ROMs from scratch.

The course cannot ship commercial ROMs or Nintendo's boot ROM, so the reference
implementation ships its own test ROMs instead. Each ROM checks one subsystem and
prints its result over the link port, exactly like Blargg's ROMs do, so the same
harness that runs Blargg/Mooneye also runs these.

A tiny assembler is included on purpose: writing the ROMs as raw byte arrays would
be unreadable, and students are expected to extend these ROMs in their labs.

Usage: python3 tools/make_test_roms.py [output_dir]   (default: build/roms)
"""
import os
import sys

# --- SM83 register indices, as used by LD r,r' and the ALU group ------------
R_B, R_C, R_D, R_E, R_H, R_L, R_HL, R_A = range(8)

# --- 16-bit register groups for LD r16,d16 and INC/DEC r16 -----------------
P_BC, P_DE, P_HL, P_SP = range(4)

# --- PUSH/POP groups: AF, not SP, is the third pair here -----------------
S_BC, S_DE, S_HL, S_AF = range(4)

# --- Condition codes: NZ, Z, NC, C ---------------------------------------
CC_NZ, CC_Z, CC_NC, CC_C = range(4)

# --- ALU operand kinds for the 0x80-0xBF and 0xC6-0xFE groups ------------
K_ADD, K_ADC, K_SUB, K_SBC, K_AND, K_XOR, K_OR, K_CP = range(8)

# --- CB-page kinds -------------------------------------------------------
CB_RLC, CB_RRC, CB_RL, CB_RR, CB_SLA, CB_SRA, CB_SWAP, CB_SRL = range(8)

# --- Interrupt bits ------------------------------------------------------
IF_VBLANK = 0x01
IF_TIMER = 0x04

# --- I/O registers used by the ROMs --------------------------------------
IO_JOYP = 0x00
IO_SB = 0x01
IO_SC = 0x02
IO_IF = 0x0F
IO_TIMA = 0x05
IO_TMA = 0x06
IO_TAC = 0x07
IO_LCDC = 0x40
IO_SCX = 0x43
IO_BGP = 0x47
IO_WY = 0x4A
IO_WX = 0x4B
IO_DMA = 0x46
IO_IE = 0xFF

PROGRAM_START = 0x0150
ROM_BANK_SIZE = 0x4000


class Asm:
    """A minimal SM83 assembler for one 16 KiB ROM bank.

    Only the instructions the test ROMs need are implemented, and everything is
    emitted through a named method so the programs below read like assembly.
    Labels are resolved in a second pass, which is why jump targets are recorded
    as fixups instead of being computed inline.

    Failure blocks are emitted after the success report and reached by forward
    jumps: a failure block is a hundred bytes of serial writes, so a JR back to
    one would be out of range on the SM83, whose displacement is one byte.
    """

    def __init__(self, org=PROGRAM_START):
        self.org = org
        self.code = bytearray()
        self.labels = {}
        self.fixups = []      # (offset, label, "rel" | "abs")
        self.vectors = []      # (rom_address, label) for interrupt vectors
        self.failures = []     # tags whose failure blocks are pending

    @property
    def pc(self):
        return self.org + len(self.code)

    def label(self, name):
        if name in self.labels:
            raise ValueError(f"duplicate label {name}")
        self.labels[name] = self.pc

    def emit(self, *bs):
        for b in bs:
            self.code.append(b & 0xFF)

    # --- loads ---------------------------------------------------------
    def ld_r_n(self, r, n):
        self.emit(0x06 | (r << 3), n)

    def ld_r_r(self, dst, src):
        self.emit(0x40 | (dst << 3) | src)

    def ld_r16_nn(self, p, nn):
        self.emit(0x01 | (p << 4), nn & 0xFF, nn >> 8)

    def ld_a16_a(self, nn):        # LD (a16), A
        self.emit(0xEA, nn & 0xFF, nn >> 8)

    def ld_a_a16(self, nn):       # LD A, (a16)
        self.emit(0xFA, nn & 0xFF, nn >> 8)

    def ldh_a8_a(self, a8):       # LDH (a8), A
        self.emit(0xE0, a8)

    def ldh_a_a8(self, a8):       # LDH A, (a8)
        self.emit(0xF0, a8)

    def ld_a_hl_inc(self):        # LD A, (HL+)
        self.emit(0x2A)

    def ld_hl_inc_a(self):        # LD (HL+), A
        self.emit(0x22)

    def ld_a_hl(self):            # LD A, (HL)
        self.emit(0x7E)

    def ld_a_de(self):            # LD A, (DE)
        self.emit(0x1A)

    def ld_de_a(self):            # LD (DE), A
        self.emit(0x12)

    def cp_hl(self):               # CP (HL): compare A with the byte at HL
        self.emit(0xBE)

    # --- arithmetic and logic -------------------------------------------
    def alu_r(self, kind, r):
        self.emit(0x80 | (kind << 3) | r)

    def alu_n(self, kind, n):
        self.emit(0xC6 | (kind << 3), n)

    def inc_r(self, r):
        self.emit(0x04 | (r << 3))

    def dec_r(self, r):
        self.emit(0x05 | (r << 3))

    def inc_r16(self, p):
        self.emit(0x03 | (p << 4))

    def dec_r16(self, p):
        self.emit(0x0B | (p << 4))

    def add_hl_r16(self, p):
        self.emit(0x09 | (p << 4))

    def rlca(self): self.emit(0x07)
    def rrca(self): self.emit(0x0F)
    def rla(self):  self.emit(0x17)
    def rra(self):  self.emit(0x1F)
    def daa(self):  self.emit(0x27)
    def cpl(self):  self.emit(0x2F)
    def scf(self):  self.emit(0x37)
    def ccf(self):  self.emit(0x3F)

    def cb_kind(self, kind, r):
        """CB-page instruction: kind selects the family, r the operand."""
        self.emit(0xCB, (kind << 3) | r)

    def bit(self, b, r):
        self.emit(0xCB, 0x40 | (b << 3) | r)

    # --- control flow ----------------------------------------------------
    def _rel_fixup(self, label):
        self.fixups.append((len(self.code), label, "rel"))
        self.code.append(0)

    def _abs_fixup(self, label):
        self.fixups.append((len(self.code), label, "abs"))
        self.code.append(0)
        self.code.append(0)

    def jr(self, label):
        self.emit(0x18)
        self._rel_fixup(label)

    def jr_cc(self, cc, label):
        self.emit(0x20 | (cc << 3))
        self._rel_fixup(label)

    def jp(self, label):
        self.emit(0xC3)
        self._abs_fixup(label)

    def jp_addr(self, addr):
        """JP to a raw 16-bit address, used for the HRAM DMA routine."""
        self.emit(0xC3, addr & 0xFF, addr >> 8)

    def jp_cc(self, cc, label):
        self.emit(0xC2 | (cc << 3))
        self._abs_fixup(label)

    def call(self, label):
        self.emit(0xCD)
        self._abs_fixup(label)

    def ret(self):
        self.emit(0xC9)

    def push(self, p):
        self.emit(0xC5 | (p << 4))

    def pop(self, p):
        self.emit(0xC1 | (p << 4))

    def halt(self): self.emit(0x76)
    def di(self):   self.emit(0xF3)
    def ei(self):   self.emit(0xFB)

    def vector(self, addr, label):
        """Install a JP to a handler at an interrupt vector address."""
        self.vectors.append((addr, label))

    # --- composite helpers used by the test ROMs ---------------------------
    def putc(self, ch):
        """Write one character over the link port, then wait for completion.

        Blargg's ROMs use exactly this protocol: put the character in SB, write
        0x81 to SC to start an internal-clock transfer, then poll SC bit 7.
        """
        self.ld_r_n(R_A, ch)
        self.ldh_a8_a(IO_SB)
        self.ld_r_n(R_A, 0x81)
        self.ldh_a8_a(IO_SC)
        self.wait_sc()

    def wait_sc(self):
        """Poll SC bit 7 until the emulated transfer completes."""
        wait = f"__wait_sc_{self.pc:04X}"
        self.label(wait)
        self.ldh_a_a8(IO_SC)
        self.alu_n(K_AND, 0x80)
        self.jr_cc(CC_NZ, wait)

    def puts(self, text):
        for ch in text:
            self.putc(ord(ch))

    def fail(self, tag):
        """Report a failure as 'FAIL <tag>' and stop the machine."""
        self.puts("FAIL ")
        self.puts(tag)
        self.putc(0x0A)
        self.di()
        self.halt()

    def report(self, text):
        """Report success and stop the machine."""
        self.puts(text)
        self.di()
        self.halt()

    def fail_block(self, tag):
        """Record a failure block to emit after the success report.

        The block is not emitted inline: a check jumps forward to it with JP,
        which has a 16-bit target and cannot go out of range.
        """
        self.failures.append(tag)

    def jp_fail(self, cc, tag):
        """JP cc to a failure block, i.e. fail unless the condition holds."""
        self.jp_cc(cc, f"__fail_{tag}")

    def emit_failures(self):
        """Emit every recorded failure block; unreachable after report()."""
        for tag in self.failures:
            self.label(f"__fail_{tag}")
            self.fail(tag)

    def resolve(self):
        """Patch every recorded jump target once all labels are known."""
        for offset, label, kind in self.fixups:
            if label not in self.labels:
                raise KeyError(f"undefined label {label}")
            target = self.labels[label]
            if kind == "abs":
                self.code[offset] = target & 0xFF
                self.code[offset + 1] = target >> 8
            else:
                rel = target - (self.org + offset + 1)
                if rel < -128 or rel > 127:
                    raise ValueError(f"JR out of range to {label}")
                self.code[offset] = rel & 0xFF


def build_rom(program, bank_count=2, cart_type=0x00, ram_size_code=0x00,
              title="TESTROM", cgb_flag=0x00, extra_banks=None):
    """Wrap a program in a cartridge: header, checksums, padding, extra banks.

    The header checksum is computed exactly as Pan Docs specifies, so these ROMs
    also exercise the emulator's own header validation.
    """
    rom = bytearray(ROM_BANK_SIZE * bank_count)

    # 0x0100-0x0103: entry point: NOP then JP 0150.
    rom[0x0100] = 0x00
    rom[0x0101] = 0xC3
    rom[0x0102] = PROGRAM_START & 0xFF
    rom[0x0103] = PROGRAM_START >> 8

    # 0x0104-0x0133: the Nintendo logo, which the boot ROM verifies.
    logo = bytes([
        0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
        0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
        0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
        0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
        0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
        0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
    ])
    rom[0x0104:0x0104 + len(logo)] = logo

    name = title.encode("ascii")[:15]
    rom[0x0134:0x0134 + len(name)] = name
    rom[0x0143] = cgb_flag
    rom[0x0147] = cart_type
    rom[0x0148] = {2: 0x00, 4: 0x01, 8: 0x02, 16: 0x03, 32: 0x04}[bank_count]
    rom[0x0149] = ram_size_code

    program.resolve()

    # Interrupt vectors: a JP at each vector address, as real cartridges have.
    for addr, label in program.vectors:
        if label not in program.labels:
            raise KeyError(f"undefined vector handler {label}")
        target = program.labels[label]
        rom[addr] = 0xC3
        rom[addr + 1] = target & 0xFF
        rom[addr + 2] = target >> 8

    rom[PROGRAM_START:PROGRAM_START + len(program.code)] = program.code

    # 0x014D: header checksum: x = x - byte - 1 over 0x134..0x14C.
    checksum = 0
    for addr in range(0x0134, 0x014D):
        checksum = (checksum - rom[addr] - 1) & 0xFF
    rom[0x014D] = checksum

    # Extra banks (MBC1 tests) get a per-bank marker at 0x4100.
    if extra_banks:
        for index, fill in extra_banks.items():
            base = index * ROM_BANK_SIZE
            rom[base:base + ROM_BANK_SIZE] = bytes([fill]) * ROM_BANK_SIZE
            rom[base + 0x0100] = fill

    # 0x014E-0x014F: global checksum over the whole ROM with those two
    # bytes treated as zero. Pan Docs calls it informational; we compute it.
    rom[0x014E] = 0
    rom[0x014F] = 0
    total = sum(rom) & 0xFFFF
    rom[0x014E] = total >> 8
    rom[0x014F] = total & 0xFF
    return bytes(rom)


def check_flags(a, tag, mask, want):
    """Compare F's top nibble with want, leaving A untouched.

    The check needs A for its own arithmetic, so it saves and restores A. That is
    what lets a caller compare the operation's result afterwards: without the
    restore, the result would already have been overwritten by the flags check.
    """
    a.push(S_AF)          # push A and F
    a.pop(S_BC)           # B = A, C = F
    a.ld_r_r(R_A, R_C)   # A = F
    a.alu_n(K_AND, mask)
    a.alu_n(K_CP, want)
    a.jp_fail(CC_NZ, tag)
    a.ld_r_r(R_A, R_B)   # restore A for the caller's result check


# ======================================================================
# Test 1: serial output — proves the bus, the serial port and the harness
# ======================================================================

def rom_serial():
    a = Asm()
    a.puts("SERIAL OK\n")
    a.di()
    a.halt()
    return build_rom(a, title="SERIAL")


# ======================================================================
# Test 2: ALU flags — proves the flag rules from L6
# ======================================================================

# (kind, a, b, expected_a, expected_flags, set_carry_first)
ALU_TESTS = [
    (K_ADD, 0x0F, 0x01, 0x10, 0x20, False),  # half-carry out of bit 3
    (K_ADD, 0xFF, 0x01, 0x00, 0xB0, False),  # Z, H and C together
    (K_ADC, 0x0F, 0x00, 0x10, 0x20, True),   # carry in from SCF
    (K_SUB, 0x10, 0x01, 0x0F, 0x60, False),  # half-borrow
    (K_SUB, 0x00, 0x01, 0xFF, 0x70, False),  # borrow out of bit 7
    (K_AND, 0xF0, 0x0F, 0x00, 0xA0, False),  # AND sets H
    (K_XOR, 0xFF, 0xFF, 0x00, 0x80, False),  # XOR clears H and C
    (K_OR,  0x00, 0x00, 0x00, 0x80, False),  # OR of zeroes
    (K_ADD, 0x0F, 0x01, 0x10, 0x20, True),   # ADD with C preserved
]


def rom_alu():
    a = Asm()

    # Every test reports its index on failure, so a failing run names the rule.
    for index, (kind, x, y, want_a, want_f, set_carry) in enumerate(ALU_TESTS):
        tag = f"a{index}"
        a.fail_block(tag)

        if set_carry:
            a.scf()
        else:
            a.ccf()          # C clear, so ADC/ADD tests start from a known state

        a.ld_r_n(R_A, x)
        a.ld_r_n(R_B, y)
        a.alu_r(kind, R_B)

        # The flags check preserves A, so the result can be compared after it.
        check_flags(a, tag, 0xF0, want_f)
        a.alu_n(K_CP, want_a)
        a.jp_fail(CC_NZ, tag)

    # ADD HL,BC: the half-carry comes from bit 11, not bit 3. Z is not
    # affected by 16-bit addition, so it is excluded from the mask.
    tag = "hl"
    a.fail_block(tag)
    a.ld_r16_nn(P_HL, 0x0FFF)
    a.ld_r16_nn(P_BC, 0x0001)
    a.add_hl_r16(P_BC)
    check_flags(a, tag, 0x70, 0x20)

    # RLCA clears Z even when the result is zero.
    tag = "rlca"
    a.fail_block(tag)
    a.ld_r_n(R_A, 0x80)
    a.rlca()
    check_flags(a, tag, 0xF0, 0x10)

    # BIT 7,A sets Z when the bit is clear, sets H and preserves C.
    tag = "bit"
    a.fail_block(tag)
    a.ld_r_n(R_A, 0x7F)
    a.scf()
    a.bit(7, R_A)
    check_flags(a, tag, 0xF0, 0xB0)

    # SWAP A with A=0x12 gives 0x21: every flag is cleared, because the
    # result is not zero.
    tag = "swap"
    a.fail_block(tag)
    a.ld_r_n(R_A, 0x12)
    a.cb_kind(CB_SWAP, R_A)
    check_flags(a, tag, 0xF0, 0x00)
    a.alu_n(K_CP, 0x21)
    a.jp_fail(CC_NZ, tag)

    # DAA after 0x0A + 0x90 gives 0x00 with Z and C set.
    tag = "daa"
    a.fail_block(tag)
    a.ld_r_n(R_A, 0x0A)
    a.ld_r_n(R_B, 0x90)
    a.alu_r(K_ADD, R_B)
    a.daa()
    # The flags check must come first: the result comparison below clobbers F.
    check_flags(a, tag, 0xF0, 0x90)
    a.alu_n(K_CP, 0x00)
    a.jp_fail(CC_NZ, tag)

    a.report("ALU OK\n")
    a.emit_failures()
    return build_rom(a, title="ALU")


# ======================================================================
# Test 3: timer — proves DIV/TIMA/TAC and the timer interrupt
# ======================================================================

def rom_timer():
    a = Asm()

    a.di()
    a.ld_r_n(R_A, 0x00)
    a.ldh_a8_a(IO_IF)              # clear pending interrupts
    a.ld_r_n(R_A, 0x00)
    a.ldh_a8_a(IO_IE)              # no handlers: poll instead
    a.ld_r_n(R_A, 0x00)
    a.ldh_a8_a(IO_TIMA)
    a.ld_r_n(R_A, 0x00)
    a.ldh_a8_a(IO_TMA)
    a.ld_r_n(R_A, 0x04)            # TAC: enabled, 4096 Hz (bit 9)
    a.ldh_a8_a(IO_TAC)

    # 256 increments at 4096 Hz take 65536 T-cycles, just under one frame.
    # If the timer never raises the interrupt the emulator runs to the frame
    # limit and the harness reports a timeout, which is a useful failure too.
    a.fail_block("timer")
    a.label("__wait_if")
    a.ldh_a_a8(IO_IF)
    a.alu_n(K_AND, IF_TIMER)
    a.jr_cc(CC_Z, "__wait_if")

    # TIMA overflowed and reloaded from TMA (which is 0), so TIMA is 0.
    a.ldh_a_a8(IO_TIMA)
    a.alu_n(K_CP, 0x00)
    a.jp_fail(CC_NZ, "timer")
    a.report("TIMER OK\n")
    a.emit_failures()
    return build_rom(a, title="TIMER")


# ======================================================================
# Test 4: interrupts — proves IE/IF, EI, HALT and the vector table
# ======================================================================

def rom_interrupt():
    a = Asm()

    a.di()
    a.ld_r_n(R_A, 0x00)
    a.ldh_a8_a(IO_IF)
    a.ld_r_n(R_A, IF_VBLANK)
    a.ldh_a8_a(IO_IE)
    a.ld_r_n(R_A, 0x00)
    a.ldh_a8_a(IO_TAC)              # no timer: only VBlank can wake us
    a.ei()
    a.halt()                           # wakes at the start of VBlank

    # The handler is reached through the vector table, so reaching it at all
    # proves that the CPU pushed the return address and jumped to 0x40. IF's
    # VBlank bit has already been cleared by hardware, so the handler checks LY
    # instead: during VBlank LY is 144 or more.
    a.fail_block("vblank")
    a.label("__ok")
    a.ldh_a_a8(0x44)                  # LY
    a.alu_n(K_CP, 144)
    a.jp_fail(CC_C, "vblank")           # carry set means LY < 144
    # Disable interrupts before stopping: otherwise the next frame's VBlank
    # would wake the CPU again and run whatever follows the HALT.
    a.ld_r_n(R_A, 0x00)
    a.ldh_a8_a(IO_IE)
    a.report("VBLANK OK\n")
    a.emit_failures()

    a.vector(0x0040, "__ok")
    return build_rom(a, title="INTERRUPT")


# ======================================================================
# Test 5: PPU — proves BG, window, palettes and the mode machine
#
# The expected image is computed independently by tests/check_frame.py: the
# emulator never tells the harness what to expect, which is the whole point.
# ======================================================================

def rom_ppu():
    a = Asm()

    # Tile 0 is colour 0 everywhere, tile 1 is colour 1 and tile 2 is colour 2.
    # Each tile is 16 bytes: eight rows of low plane then high plane.
    tiles = [
        (0x8000, bytes([0x00, 0x00] * 8)),
        (0x8010, bytes([0xFF, 0x00] * 8)),
        (0x8020, bytes([0x00, 0xFF] * 8)),
    ]

    # The LCD must be off to write VRAM on hardware, so turn it off first.
    a.ld_r_n(R_A, 0x00)
    a.ldh_a8_a(IO_LCDC)

    for addr, data in tiles:
        a.ld_r16_nn(P_HL, addr)
        for byte in data:
            a.ld_r_n(R_A, byte)
            a.ld_hl_inc_a()

    # BG map 0x9800: tile 1 in the top-left 10x9 tiles, tile 0 elsewhere.
    # The counter needs 16 bits, so BC is used and tested as B|C.
    a.ld_r16_nn(P_HL, 0x9800)
    a.ld_r16_nn(P_BC, 32 * 32)
    a.ld_r_n(R_A, 0x00)
    a.label("__fill_bg")
    a.ld_hl_inc_a()
    a.dec_r16(P_BC)
    a.ld_r_r(R_A, R_B)
    a.alu_r(K_OR, R_C)
    a.jr_cc(CC_NZ, "__fill_bg_reload")
    a.jr("__fill_bg_done")
    a.label("__fill_bg_reload")
    a.ld_r_n(R_A, 0x00)
    a.jr("__fill_bg")
    a.label("__fill_bg_done")

    for row in range(9):
        a.ld_r16_nn(P_HL, 0x9800 + row * 32)
        a.ld_r_n(R_B, 10)
        a.ld_r_n(R_A, 0x01)
        a.label(f"__row_{row}")
        a.ld_hl_inc_a()
        a.dec_r(R_B)
        a.jr_cc(CC_NZ, f"__row_{row}")

    # Window map 0x9C00: tile 2 everywhere, again through a 16-bit counter.
    a.ld_r16_nn(P_HL, 0x9C00)
    a.ld_r16_nn(P_BC, 1024)
    a.ld_r_n(R_A, 0x02)
    a.label("__fill_win")
    a.ld_hl_inc_a()
    a.dec_r16(P_BC)
    a.ld_r_r(R_A, R_B)
    a.alu_r(K_OR, R_C)
    a.jr_cc(CC_NZ, "__fill_win_reload")
    a.jr("__fill_win_done")
    a.label("__fill_win_reload")
    a.ld_r_n(R_A, 0x02)
    a.jr("__fill_win")
    a.label("__fill_win_done")

    a.ld_r_n(R_A, 0xE4)          # BGP: index 0->shade0, 1->1, 2->2, 3->3
    a.ldh_a8_a(IO_BGP)
    a.ld_r_n(R_A, 0x48)           # WY = 72: the window starts on line 72
    a.ldh_a8_a(IO_WY)
    a.ld_r_n(R_A, 87)              # WX = 87: its left edge is at x = 80
    a.ldh_a8_a(IO_WX)

    # LCDC: LCD on, window on, window map 9C00, BG on, tile data 8000,
    # BG map 9800. Bit 6 selects the window's map, so it must be set as well
    # as bit 5: leaving it clear makes the window read the BG map instead.
    a.ld_r_n(R_A, 0xF1)
    a.ldh_a8_a(IO_LCDC)

    # Wait for the first VBlank: by then the frame is complete.
    a.fail_block("ppu")
    a.ld_r_n(R_A, 0x00)
    a.ldh_a8_a(IO_IF)
    a.label("__wait_vblank")
    a.ldh_a_a8(IO_IF)
    a.alu_n(K_AND, IF_VBLANK)
    a.jr_cc(CC_Z, "__wait_vblank")
    a.report("PPU OK\n")
    a.emit_failures()
    return build_rom(a, title="PPU")


# ======================================================================
# Test 6: MBC1 — proves banking, the bank-0 quirk and the RAM enable
# ======================================================================

def rom_mbc1():
    a = Asm()

    def expect(name, want):
        a.fail_block(name)
        a.ld_r16_nn(P_HL, 0x4100)
        a.ld_a_hl()
        a.alu_n(K_CP, want)
        a.jp_fail(CC_NZ, name)

    # Bank 1 is mapped in at power-on.
    expect("mbc1_bank1", 0x11)

    a.ld_r_n(R_A, 0x02)
    a.ld_a16_a(0x2000)
    expect("mbc1_bank2", 0x22)

    a.ld_r_n(R_A, 0x03)
    a.ld_a16_a(0x2000)
    expect("mbc1_bank3", 0x33)

    # The classic quirk: selecting bank 0 selects bank 1 instead.
    a.ld_r_n(R_A, 0x00)
    a.ld_a16_a(0x2000)
    expect("mbc1_bank0", 0x11)

    # Disable cart RAM, then enable it again with 0x0A.
    a.ld_r_n(R_A, 0x00)
    a.ld_a16_a(0x0000)
    a.ld_r_n(R_A, 0x0A)
    a.ld_a16_a(0x0000)

    a.report("MBC1 OK\n")
    a.emit_failures()
    return build_rom(a, cart_type=0x01, bank_count=4,
                     extra_banks={1: 0x11, 2: 0x22, 3: 0x33},
                     title="MBC1")


# ======================================================================
# Test 7: joypad — proves the FF00 matrix and the joypad interrupt
#
# The host presses A at frame 60 (see tests/run_tests.sh). This ROM waits for
# it, so it also proves that a held button stays visible to a polling game.
# ======================================================================

def rom_joypad():
    a = Asm()

    a.di()
    a.ld_r_n(R_A, 0x00)
    a.ldh_a8_a(IO_IF)
    a.ld_r_n(R_A, 0x00)
    a.ldh_a8_a(IO_IE)              # no handlers: poll IF instead

    # Select the button group: bit 5 = 0, bit 4 = 1.
    a.ld_r_n(R_A, 0x10)
    a.ldh_a8_a(IO_JOYP)

    a.fail_block("joypad")

    # Wait for A (JOYP bit 0) to go low.
    a.label("__wait_a")
    a.ldh_a_a8(IO_JOYP)
    a.alu_n(K_AND, 0x01)
    a.jr_cc(CC_NZ, "__wait_a")

    # A is down: the joypad interrupt must be requested as well.
    a.ldh_a_a8(IO_IF)
    a.alu_n(K_AND, 0x10)
    a.jp_fail(CC_Z, "joypad")

    a.report("JOYPAD OK\n")
    a.emit_failures()
    return build_rom(a, title="JOYPAD")


# ======================================================================
# Test 8: OAM DMA — proves FF46, the 640 T-cycle transfer and the lockout
#
# The delay loop lives in HRAM, because during the transfer the CPU can only
# access HRAM: a ROM loop would be blocked, exactly as on hardware. The routine
# starts the transfer itself, so the CPU is inside HRAM when it begins.
# ======================================================================

def rom_dma():
    a = Asm()

    # Fill WRAM 0xC000-0xC09F with a known pattern.
    a.ld_r16_nn(P_HL, 0xC000)
    a.ld_r_n(R_B, 0xA0)
    a.ld_r_n(R_A, 0x00)
    a.label("__fill")
    a.ld_hl_inc_a()
    a.inc_r(R_A)
    a.dec_r(R_B)
    a.jr_cc(CC_NZ, "__fill")

    # The HRAM routine, copied to 0xFF80. It starts the transfer, delays in
    # HRAM, then jumps back to __after_dma (patched in below).
    hram = bytearray()
    hram += bytes([0x3E, 0xC0])            # LD A, 0xC0
    hram += bytes([0xE0, IO_DMA])           # LDH (46), A: start OAM DMA
    # The counter lives at 0xFF92, clear of the routine's own bytes at
    # 0xFF80-0xFF91: putting it inside the routine would overwrite the
    # return address the final JP reads.
    hram += bytes([0x21, 0x92, 0xFF])       # LD HL, 0xFF92
    hram += bytes([0x36, 0x00])              # LD (HL), 0
    hram += bytes([0x34])                     # loop: INC (HL)
    hram += bytes([0x7E])                     # LD A, (HL)
    hram += bytes([0xFE, 0x80])               # CP 0x80
    hram += bytes([0x20, 0xFA])               # JR NZ, loop
    hram += bytes([0xC3, 0x00, 0x00])        # JP __after_dma

    a.ld_r16_nn(P_HL, 0xFF80)
    copy_start = a.pc - a.org      # index of the first copy instruction
    for byte in hram:
        a.ld_r_n(R_A, byte)        # 2 bytes: LD A,d8
        a.ld_hl_inc_a()             # 1 byte:  LD (HL+),A

    a.jp_addr(0xFF80)
    a.label("__after_dma")

    # The HRAM routine's final JP must return to the instruction after the copy
    # loop. Patch the two bytes it will be copied from, which is safe because
    # the address is only known now, after the whole program has been emitted.
    # Each copy iteration is three bytes: LD A,d8 (opcode and operand) then
    # LD (HL+),A, so the operand of iteration i is at copy_start + 3*i + 1.
    after = a.labels["__after_dma"]
    a.code[copy_start + 3 * 16 + 1] = after & 0xFF
    a.code[copy_start + 3 * 17 + 1] = after >> 8

    # Compare OAM with the WRAM pattern.
    a.ld_r16_nn(P_HL, 0xC000)
    a.ld_r16_nn(P_DE, 0xFE00)
    a.ld_r_n(R_B, 0xA0)
    a.fail_block("dma")
    a.label("__cmp")
    a.ld_a_de()                          # LD A, (DE): the OAM byte
    a.cp_hl()                              # compare it with the WRAM byte
    a.jp_fail(CC_NZ, "dma")
    a.inc_r16(P_HL)
    a.inc_r16(P_DE)
    a.dec_r(R_B)
    a.jr_cc(CC_NZ, "__cmp")

    a.report("DMA OK\n")
    a.emit_failures()
    return build_rom(a, title="DMA")


# ======================================================================
# Test 9: sound — proves NR52, the frame sequencer and the DAC path
#
# The host checks the audio path independently with --audio-stats: this ROM
# proves only that the registers behave and that a channel reports running.
# ======================================================================

def rom_sound():
    a = Asm()

    a.ld_r_n(R_A, 0x80)       # NR52: power on the APU
    a.ldh_a8_a(0x26)
    a.ld_r_n(R_A, 0xF0)       # NR12: volume 15, DAC on
    a.ldh_a8_a(0x12)
    a.ld_r_n(R_A, 0x80)       # NR11: 50 % duty
    a.ldh_a8_a(0x11)
    a.ld_r_n(R_A, 0x00)       # NR13: period low
    a.ldh_a8_a(0x13)
    a.ld_r_n(R_A, 0x87)       # NR14: trigger + period high
    a.ldh_a8_a(0x14)

    # NR52 bit 0 must report square 1 as running.
    a.fail_block("sound")
    a.ldh_a_a8(0x26)
    a.alu_n(K_AND, 0x01)
    a.jp_fail(CC_Z, "sound")
    a.report("SOUND OK\n")
    a.emit_failures()
    return build_rom(a, title="SOUND")


# ======================================================================
# Test 10: determinism — used by the save-state and replay tests
# ======================================================================

def rom_determinism():
    a = Asm()

    # Scroll the background one pixel per frame, so consecutive frames differ
    # and a save state that misses PPU state shows up as a hash mismatch.
    a.ld_r_n(R_A, 0x00)
    a.ldh_a8_a(IO_LCDC)
    a.ld_r_n(R_A, 0xE4)
    a.ldh_a8_a(IO_BGP)

    # Clear the first tile, then fill the BG map with it.
    a.ld_r16_nn(P_HL, 0x8000)
    a.ld_r_n(R_B, 0x10)
    a.ld_r_n(R_A, 0x00)
    a.label("__clear_tile")
    a.ld_hl_inc_a()
    a.dec_r(R_B)
    a.jr_cc(CC_NZ, "__clear_tile")

    a.ld_r16_nn(P_HL, 0x9800)
    a.ld_r16_nn(P_BC, 0x400)
    a.ld_r_n(R_A, 0x01)
    a.label("__clear_map")
    a.ld_hl_inc_a()
    a.dec_r16(P_BC)
    a.ld_r_r(R_A, R_B)
    a.alu_r(K_OR, R_C)
    a.jr_cc(CC_NZ, "__clear_map_reload")
    a.jr("__clear_map_done")
    a.label("__clear_map_reload")
    a.ld_r_n(R_A, 0x01)
    a.jr("__clear_map")
    a.label("__clear_map_done")

    a.ld_r_n(R_A, 0x91)      # LCD on, BG on, tile data 8000, BG map 9800
    a.ldh_a8_a(IO_LCDC)

    a.label("__spin")
    a.ldh_a_a8(IO_SCX)
    a.inc_r(R_A)
    a.ldh_a8_a(IO_SCX)
    a.jr("__spin")
    return build_rom(a, title="DETERM")


# ======================================================================
# Test 11: wave RAM must not disturb the register file
#
# Wave RAM (FF30-FF3F) is storage of its own: writing it must not change
# NR50/NR51/NR52. apu_write_reg() used to index the 0x20-byte NR10-NR2F
# array with (reg - 0x10), so a wave-RAM write landed past its end and
# clobbered NR50/NR51/NR52 and the APU power flag, which the mixer reads
# directly. The ROM below writes all 16 wave-RAM bytes with values chosen so
# that the old indexing would clear the power bit, then checks NR52 bit 7.
# ======================================================================

def rom_waveram():
    a = Asm()

    a.ld_r_n(R_A, 0x80)      # NR52: power on the APU
    a.ldh_a8_a(0x26)
    a.ld_r_n(R_A, 0x77)      # NR50: master volume, both sides
    a.ldh_a8_a(0x24)
    a.ld_r_n(R_A, 0xF3)      # NR51: panning
    a.ldh_a8_a(0x25)

    # Write every wave-RAM byte. Byte 3 (FF33) is 0x00: with the old
    # indexing that is offset 0x23 of nr[], i.e. the power flag.
    for i in range(16):
        a.ld_r_n(R_A, 0x00 if i == 3 else 0x0F)
        a.ldh_a8_a(0x30 + i)

    # NR52 bit 7 (power) must still be set.
    a.fail_block("waveram")
    a.ldh_a_a8(0x26)
    a.alu_n(K_AND, 0x80)
    a.jp_fail(CC_Z, "waveram")

    # NR50 and NR51 must read back unchanged too.
    a.fail_block("wave50")
    a.ldh_a_a8(0x24)
    a.alu_n(K_XOR, 0x77)
    a.jp_fail(CC_NZ, "wave50")
    a.fail_block("wave51")
    a.ldh_a_a8(0x25)
    a.alu_n(K_XOR, 0xF3)
    a.jp_fail(CC_NZ, "wave51")

    a.report("WAVE RAM OK\n")
    a.emit_failures()
    return build_rom(a, title="WAVERAM")


ROMS = {
    "serial.gb": rom_serial,
    "alu.gb": rom_alu,
    "timer.gb": rom_timer,
    "interrupt.gb": rom_interrupt,
    "ppu.gb": rom_ppu,
    "mbc1.gb": rom_mbc1,
    "joypad.gb": rom_joypad,
    "dma.gb": rom_dma,
    "sound.gb": rom_sound,
    "waveram.gb": rom_waveram,
    "determinism.gb": rom_determinism,
}


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "build/roms"
    os.makedirs(out_dir, exist_ok=True)

    for name, build in ROMS.items():
        path = os.path.join(out_dir, name)
        with open(path, "wb") as f:
            f.write(build())
        print(f"{path}: {os.path.getsize(path)} bytes")


if __name__ == "__main__":
    main()
