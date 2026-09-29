/* test_flags.c — unit tests for the register file and the flag rules (L2, L6).
 *
 * These tests need no bus, no ROM and no window: they instantiate a GB on the
 * stack and drive cpu_step() over hand-written programs. That is the point of
 * keeping the core free of SDL and of globals.
 *
 * Build: cmake --build build --target gb_tests && ./build/gb_tests
 */
#include "cpu.h"
#include "gb.h"
#include "mmu.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                            \
    do {                                                            \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

/* wram8 — the byte at a WRAM address, in the bank the bus currently selects.
 *
 * The tests must not index a fixed bank directly: the bus maps 0xC000-0xCFFF to
 * bank 0 and 0xD000-0xDFFF to the SVBK bank (bank 1 on the DMG, which has no
 * bank register), so a direct index would disagree with the bus and every test
 * would silently read zeros. Going through the same mapping the bus uses is what
 * makes these tests evidence about the CPU rather than about the test's own mistake.
 */
static uint8_t *wram8(GB *gb, uint16_t addr)
{
    unsigned bank = (addr < 0xD000u) ? 0u
                                   : (gb->mmu.cgb ? gb->mmu.wram_bank : 1u);
    return &gb->mmu.wram[bank][addr & 0x0FFFu];
}

/* Load a program into WRAM, point PC at it, and run it to the end.
 *
 * The loop stops when PC has walked past the last byte, so a test can write a
 * straight-line program and read the final state. It also sums the cycles, which
 * is what the cycle-count tests check. */
static uint32_t run(GB *gb, const uint8_t *code, size_t len)
{
    memcpy(wram8(gb, 0xC100), code, len);
    gb->cpu.pc = 0xC100;

    uint32_t total = 0;
    uint16_t end = (uint16_t)(0xC100 + len);
    while (gb->cpu.pc < end)
        total += cpu_step(&gb->cpu, &gb->mmu);
    return total;
}

static void test_flag_masking(void)
{
    CPU c;
    memset(&c, 0, sizeof(c));

    /* The low nibble of F does not exist in silicon. The only way the CPU
     * writes F is POP AF, which masks it, and test_stack_and_pop_af checks
     * that. Here we check the other half of the contract: setting one flag
     * must leave the other three exactly as they were. */
    c.f = 0xF0;
    cpu_set_flag(&c, FLAG_Z, false);
    CHECK((c.f & FLAG_Z) == 0, "clearing Z must not disturb the other flags");
    CHECK((c.f & FLAG_C) != 0, "clearing Z must not clear C");

    cpu_set_flag(&c, FLAG_H, false);
    CHECK((c.f & FLAG_H) == 0, "clearing H must take effect");
    CHECK((c.f & FLAG_N) != 0, "clearing H must not clear N");
}

static void test_pair_aliasing(void)
{
    CPU c;
    memset(&c, 0, sizeof(c));

    c.hl = 0x1234;
    CHECK(c.h == 0x12 && c.l == 0x34, "HL must alias H and L");
    c.h = 0xAB;
    CHECK(c.hl == 0xAB34, "writing H must change HL, got %04X", c.hl);
    c.af = 0xFFFF;
    CHECK(c.a == 0xFF && c.f == 0xFF, "AF must alias A and F, got F=%02X", c.f);
}

static void test_alu_flags(GB *gb)
{
    /* ADD A,B with A=0x0F, B=0x01: half-carry out of bit 3. */
    const uint8_t add_h[] = { 0x3E, 0x0F, 0x06, 0x01, 0x80 };
    run(gb, add_h, sizeof(add_h));
    CHECK(gb->cpu.a == 0x10, "ADD 0F+01: A=%02X, want 10", gb->cpu.a);
    CHECK(gb->cpu.f == 0x20, "ADD 0F+01: F=%02X, want 20 (H)", gb->cpu.f);

    /* ADD A,B with A=0xFF, B=0x01: Z, H and C together. */
    const uint8_t add_all[] = { 0x3E, 0xFF, 0x06, 0x01, 0x80 };
    run(gb, add_all, sizeof(add_all));
    CHECK(gb->cpu.a == 0x00 && gb->cpu.f == 0xB0,
          "ADD FF+01: A=%02X F=%02X, want 00 and B0", gb->cpu.a, gb->cpu.f);

    /* SUB A,B with A=0x10, B=0x01: half-borrow, N set. */
    const uint8_t sub[] = { 0x3E, 0x10, 0x06, 0x01, 0x90 };
    run(gb, sub, sizeof(sub));
    CHECK(gb->cpu.a == 0x0F && gb->cpu.f == 0x60,
          "SUB 10-01: A=%02X F=%02X, want 0F and 60", gb->cpu.a, gb->cpu.f);

    /* AND A,B with A=0xF0, B=0x0F: Z and H set, N and C clear. */
    const uint8_t and_[] = { 0x3E, 0xF0, 0x06, 0x0F, 0xA0 };
    run(gb, and_, sizeof(and_));
    CHECK(gb->cpu.a == 0x00 && gb->cpu.f == 0xA0,
          "AND F0,0F: A=%02X F=%02X, want 00 and A0", gb->cpu.a, gb->cpu.f);

    /* INC A with A=0x0F must set H and leave C alone. C is set first. */
    const uint8_t inc_h[] = { 0x37, 0x3E, 0x0F, 0x3C };
    run(gb, inc_h, sizeof(inc_h));
    CHECK(gb->cpu.a == 0x10, "INC A: A=%02X, want 10", gb->cpu.a);
    CHECK(gb->cpu.f == 0x30, "INC A: F=%02X, want 30 (C preserved)", gb->cpu.f);

    /* RLCA with A=0x80 must clear Z even though the result is zero. */
    const uint8_t rlca[] = { 0x3E, 0x80, 0x07 };
    run(gb, rlca, sizeof(rlca));
    CHECK(gb->cpu.a == 0x01 && gb->cpu.f == 0x10,
          "RLCA: A=%02X F=%02X, want 01 and 10", gb->cpu.a, gb->cpu.f);

    /* BIT 7,A with A=0x7F and C set: Z and H set, C preserved. */
    const uint8_t bit[] = { 0x37, 0x3E, 0x7F, 0xCB, 0x7F };
    run(gb, bit, sizeof(bit));
    CHECK(gb->cpu.f == 0xB0, "BIT 7,A: F=%02X, want B0", gb->cpu.f);

    /* DAA after 0x0A + 0x90: A=0x00 with Z and C set. */
    const uint8_t daa[] = { 0x3E, 0x0A, 0x06, 0x90, 0x80, 0x27 };
    run(gb, daa, sizeof(daa));
    CHECK(gb->cpu.a == 0x00 && gb->cpu.f == 0x90,
          "DAA: A=%02X F=%02X, want 00 and 90", gb->cpu.a, gb->cpu.f);

    /* ADD HL,BC with HL=0x0FFF, BC=0x0001: half-carry from bit 11. */
    const uint8_t add_hl[] = { 0x21, 0xFF, 0x0F, 0x01, 0x01, 0x00, 0x09 };
    run(gb, add_hl, sizeof(add_hl));
    CHECK(gb->cpu.hl == 0x1000, "ADD HL,BC: HL=%04X, want 1000", gb->cpu.hl);
    CHECK((gb->cpu.f & 0x70) == 0x20, "ADD HL,BC: F=%02X, want H set", gb->cpu.f);
}

static void test_cycle_counts(GB *gb)
{
    /* NOP is 4 T-cycles, LD A,d8 is 8, LD BC,d16 is 12. */
    const uint8_t nop[] = { 0x00 };
    CHECK(run(gb, nop, sizeof(nop)) == 4, "NOP must take 4 T-cycles");

    const uint8_t ld_a[] = { 0x3E, 0x42 };
    CHECK(run(gb, ld_a, sizeof(ld_a)) == 8, "LD A,d8 must take 8 T-cycles");

    const uint8_t ld_bc[] = { 0x01, 0x34, 0x12 };
    CHECK(run(gb, ld_bc, sizeof(ld_bc)) == 12, "LD BC,d16 must take 12 T-cycles");

    /* LD (HL),A costs 8 T-cycles, not 4: (HL) is a memory access. */
    const uint8_t ld_hl_a[] = { 0x21, 0x00, 0xC0, 0x77 };
    memcpy(wram8(gb, 0xC100), ld_hl_a, sizeof(ld_hl_a));
    gb->cpu.pc = 0xC100;
    gb->cpu.hl = 0;
    CHECK(cpu_step(&gb->cpu, &gb->mmu) == 12, "LD HL,d16 must take 12 T-cycles");
    CHECK(gb->cpu.hl == 0xC000, "LD HL,d16 must load HL, got %04X", gb->cpu.hl);
    CHECK(cpu_step(&gb->cpu, &gb->mmu) == 8, "LD (HL),A must take 8 T-cycles");
    CHECK(*wram8(gb, 0xC000) == gb->cpu.a, "LD (HL),A must write memory");

    /* JR taken costs 12 T-cycles, not taken costs 8. */
    const uint8_t jr[] = { 0x18, 0x02 };
    memcpy(wram8(gb, 0xC200), jr, sizeof(jr));
    gb->cpu.pc = 0xC200;
    CHECK(cpu_step(&gb->cpu, &gb->mmu) == 12, "JR taken must take 12 T-cycles");
}

static void test_stack_and_pop_af(GB *gb)
{
    gb->cpu.sp = 0xC100;
    cpu_push16(&gb->cpu, &gb->mmu, 0x1234);

    CHECK(gb->cpu.sp == 0xC0FE, "PUSH must move SP down by 2, got %04X", gb->cpu.sp);
    CHECK(*wram8(gb, 0xC0FE) == 0x34 && *wram8(gb, 0xC0FF) == 0x12,
          "PUSH must store the low byte first");

    uint16_t v = cpu_pop16(&gb->cpu, &gb->mmu);
    CHECK(v == 0x1234, "POP must return what PUSH stored, got %04X", v);
    CHECK(gb->cpu.sp == 0xC100, "POP must restore SP");

    /* POP AF must mask the low nibble of F: 0xFFFF becomes 0xFFF0. */
    gb->cpu.sp = 0xC100;
    cpu_push16(&gb->cpu, &gb->mmu, 0xFFFF);
    const uint8_t pop_af[] = { 0xF1 };
    run(gb, pop_af, sizeof(pop_af));
    CHECK(gb->cpu.af == 0xFFF0, "POP AF must mask the low nibble, got %04X", gb->cpu.af);
}

static void test_interrupts(GB *gb)
{
    /* An interrupt must be serviced between instructions, not inside one. */
    gb->cpu.pc = 0xC100;
    gb->cpu.ime = true;
    gb->mmu.ie = IF_VBLANK;
    gb->mmu.if_reg = IF_VBLANK;
    gb->cpu.sp = 0xC100;

    const uint8_t nop[] = { 0x00 };
    memcpy(wram8(gb, 0xC100), nop, sizeof(nop));

    uint32_t cycles = cpu_step(&gb->cpu, &gb->mmu);
    CHECK(cycles == 20, "an interrupt must cost 20 T-cycles, got %u", cycles);
    CHECK(gb->cpu.pc == 0x0040, "VBlank must vector to 0040, got %04X", gb->cpu.pc);
    CHECK(!gb->cpu.ime, "servicing an interrupt must clear IME");
    CHECK((gb->mmu.if_reg & IF_VBLANK) == 0, "the CPU must clear the IF bit");
    CHECK(cpu_pop16(&gb->cpu, &gb->mmu) == 0xC100, "the return address must be pushed");

    /* With IME clear, a pending interrupt must not be serviced. */
    gb->cpu.pc = 0xC100;
    gb->cpu.ime = false;
    gb->mmu.if_reg = IF_VBLANK;
    gb->cpu.sp = 0xC100;
    uint32_t ignored = cpu_step(&gb->cpu, &gb->mmu);
    CHECK(ignored == 4, "without IME the instruction must just run, got %u", ignored);
    CHECK(gb->cpu.pc == 0xC101, "without IME PC must advance, got %04X", gb->cpu.pc);
}

static void test_halt_bug(GB *gb)
{
    /* HALT with IME clear and an interrupt pending must not halt: it must
     * fail to advance PC for one instruction, which is the HALT bug. */
    gb->cpu.pc = 0xC100;
    gb->cpu.ime = false;
    gb->mmu.ie = IF_VBLANK;
    gb->mmu.if_reg = IF_VBLANK;

    const uint8_t program[] = { 0x76, 0x3E, 0x42 };   /* HALT; LD A,42 */
    memcpy(wram8(gb, 0xC100), program, sizeof(program));

    cpu_step(&gb->cpu, &gb->mmu);
    CHECK(gb->cpu.halt_bug, "HALT with IME clear and IF&IE set must set the bug");
    CHECK(!gb->cpu.halted, "the HALT bug must not halt the CPU");

    /* The next instruction is fetched without advancing PC, so the byte at
     * 0xC101 is executed twice: first as the opcode (0x3E = LD A,d8) and then
     * as its operand, which is why A ends up holding 0x3E and PC has advanced
     * past it. That double execution IS the HALT bug. */
    cpu_step(&gb->cpu, &gb->mmu);
    CHECK(gb->cpu.a == 0x3E, "the byte at PC must be executed twice, A=%02X", gb->cpu.a);
    CHECK(gb->cpu.pc == 0xC102, "PC must advance past the re-read byte, PC=%04X",
          gb->cpu.pc);
}

static void test_ei_delay(GB *gb)
{
    /* EI must not enable interrupts before the following instruction runs. */
    gb->cpu.pc = 0xC100;
    gb->cpu.ime = false;
    gb->cpu.ime_pending = false;
    gb->mmu.ie = IF_VBLANK;
    gb->mmu.if_reg = IF_VBLANK;
    gb->cpu.sp = 0xC100;

    const uint8_t program[] = { 0xFB, 0x00 };   /* EI; NOP */
    memcpy(wram8(gb, 0xC100), program, sizeof(program));

    uint32_t cycles = cpu_step(&gb->cpu, &gb->mmu);
    CHECK(cycles == 4 && gb->cpu.pc == 0xC101, "EI must not service an interrupt yet");
    CHECK(gb->cpu.ime_pending, "EI must arm IME for after the next instruction");

    /* The instruction after EI still runs with IME clear: the interrupt may
     * only be taken once that instruction has completed. */
    cycles = cpu_step(&gb->cpu, &gb->mmu);
    CHECK(cycles == 4, "the instruction after EI must run normally, got %u", cycles);
    CHECK(gb->cpu.pc == 0xC102, "it must complete before the interrupt, PC=%04X",
          gb->cpu.pc);

    cycles = cpu_step(&gb->cpu, &gb->mmu);
    CHECK(cycles == 20, "the interrupt must be taken next, got %u", cycles);
    CHECK(!gb->cpu.ime, "servicing the interrupt must clear IME again");
    CHECK(gb->cpu.pc == 0x0040, "the interrupt must vector to 0040, got %04X", gb->cpu.pc);
}

static void test_timer_edges(GB *gb)
{
    /* The timer must increment TIMA on a falling edge of the selected bit. */
    gb->mmu.timer.div_counter = 0;
    gb->mmu.timer.tima = 0;
    gb->mmu.timer.tma = 0;
    gb->mmu.timer.tac = 0x04;      /* enabled, bit 9 = 4096 Hz */

    /* 512 T-cycles take bit 9 from 0 to 1, i.e. no falling edge yet. */
    timer_tick(&gb->mmu.timer, 512, &gb->mmu);
    CHECK(gb->mmu.timer.tima == 0, "TIMA must not tick on a rising edge, got %02X",
          gb->mmu.timer.tima);

    /* Another 512 T-cycles take bit 9 from 1 to 0: exactly one increment. */
    timer_tick(&gb->mmu.timer, 512, &gb->mmu);
    CHECK(gb->mmu.timer.tima == 1, "TIMA must tick once per falling edge, got %02X",
          gb->mmu.timer.tima);

    /* Overflow must reload from TMA after 4 T-cycles and request the IRQ. */
    gb->mmu.timer.tima = 0xFF;
    gb->mmu.timer.tma = 0x42;
    gb->mmu.if_reg = 0;
    gb->mmu.timer.div_counter = 0;
    timer_tick(&gb->mmu.timer, 512, &gb->mmu);   /* bit 9: 0 -> 1 */
    timer_tick(&gb->mmu.timer, 512, &gb->mmu);   /* bit 9: 1 -> 0: overflow */
    CHECK(gb->mmu.timer.tima == 0x00, "TIMA must read 0 during the reload delay");
    timer_tick(&gb->mmu.timer, 4, &gb->mmu);
    CHECK(gb->mmu.timer.tima == 0x42, "TIMA must reload from TMA, got %02X",
          gb->mmu.timer.tima);
    CHECK((gb->mmu.if_reg & IF_TIMER) != 0, "the timer interrupt must be requested");

    /* Writing DIV must reset the whole 16-bit counter. */
    gb->mmu.timer.div_counter = 0x1234;
    timer_write(&gb->mmu.timer, 0xFF04, 0x00);
    CHECK(gb->mmu.timer.div_counter == 0, "writing DIV must reset the counter");
}

static void test_joypad_matrix(GB *gb)
{
    Joypad *j = &gb->mmu.joypad;

    joypad_init(j);
    /* Post-boot P1 reads 0xCF: both groups selected, nothing held. */
    CHECK(joypad_read(j) == 0xCF, "post-boot P1 must read CF, got %02X", joypad_read(j));

    /* Select the buttons group (bit 5 low) and press A (bit 0). */
    joypad_write(j, 0x10);
    joypad_set_input(j, JOY_A, 0, &gb->mmu);
    CHECK((joypad_read(j) & 0x01) == 0, "A must read as pressed in the button group");

    /* The direction group is not selected, so it must read 0xF. */
    joypad_write(j, 0x20);
    CHECK((joypad_read(j) & 0x0F) == 0x0F, "an unselected group must read F");

    /* Pressing a button raises the joypad interrupt. */
    gb->mmu.if_reg = 0;
    joypad_write(j, 0x10);
    joypad_set_input(j, JOY_B, 0, &gb->mmu);
    CHECK((gb->mmu.if_reg & IF_JOYPAD) != 0, "a press must raise the joypad interrupt");
}

static void test_open_bus_and_echo(GB *gb)
{
    /* Echo RAM must mirror WRAM, not duplicate it. */
    *wram8(gb, 0xC100) = 0x5A;
    CHECK(mmu_read8(&gb->mmu, 0xE100) == 0x5A, "E100 must mirror C100");
    mmu_write8(&gb->mmu, 0xE100, 0xA5);
    CHECK(*wram8(gb, 0xC100) == 0xA5, "a write to E100 must reach C100");

    /* The unused range must read as the last value on the bus. */
    mmu_write8(&gb->mmu, 0xC000, 0x3C);
    CHECK(mmu_read8(&gb->mmu, 0xFEA0) == 0x3C, "FEA0 must read the open bus");
}

static void test_serial(GB *gb)
{
    /* Writing 0x81 to SC starts a transfer that completes and clears bit 7,
     * which is exactly what the test harness polls for. */
    mmu_write8(&gb->mmu, 0xFF01, 'A');
    mmu_write8(&gb->mmu, 0xFF02, 0x81);
    CHECK((mmu_read8(&gb->mmu, 0xFF02) & 0x80) != 0, "SC bit 7 must be set while busy");

    mmu_tick(&gb->mmu, 4096);
    CHECK((mmu_read8(&gb->mmu, 0xFF02) & 0x80) == 0, "SC bit 7 must clear when done");
    CHECK((gb->mmu.if_reg & IF_SERIAL) != 0, "the serial interrupt must be requested");
    CHECK(strcmp(gb->mmu.serial_buf, "A") == 0, "the sent byte must be captured, got '%s'",
          gb->mmu.serial_buf);
}

static void test_oam_dma(GB *gb)
{
    /* Fill WRAM and copy it to OAM through FF46. */
    for (unsigned i = 0; i < OAM_SIZE; i++) *wram8(gb, (uint16_t)(0xC000 + i)) = (uint8_t)i;

    mmu_write8(&gb->mmu, 0xFF46, 0xC0);
    CHECK(gb->mmu.dma_active, "writing FF46 must start the transfer");

    /* While the transfer runs, only HRAM is accessible. */
    CHECK(mmu_read8(&gb->mmu, 0xC000) == 0xFF,
          "the CPU must read FF outside HRAM during OAM DMA");
    mmu_write8(&gb->mmu, 0xC000, 0x00);
    CHECK(*wram8(gb, 0xC000) == 0x00, "writes outside HRAM must be ignored");

    /* 160 bytes take 640 T-cycles. */
    mmu_tick(&gb->mmu, 640);
    CHECK(!gb->mmu.dma_active, "the transfer must finish after 640 T-cycles");

    int bad = -1;
    for (unsigned i = 0; i < OAM_SIZE; i++) {
        if (gb->mmu.oam[i] != (uint8_t)i) { bad = (int)i; break; }
    }
    CHECK(bad < 0, "OAM[%d] must be %02X, got %02X", bad,
          bad < 0 ? 0 : (uint8_t)bad, bad < 0 ? 0 : gb->mmu.oam[bad]);
}

static void test_save_state_round_trip(GB *gb)
{
    const char *path = "test.state";

    gb->cpu.a = 0x42;
    gb->cpu.hl = 0x1234;
    gb->mmu.vram[0][0x10] = 0xAB;
    gb->mmu.ppu.scx = 0x21;

    CHECK(gb_save_state(gb, path), "gb_save_state must succeed");

    gb->cpu.a = 0;
    gb->cpu.hl = 0;
    gb->mmu.vram[0][0x10] = 0;
    gb->mmu.ppu.scx = 0;

    CHECK(gb_load_state(gb, path), "gb_load_state must succeed");
    CHECK(gb->cpu.a == 0x42 && gb->cpu.hl == 0x1234, "CPU state must round-trip");
    CHECK(gb->mmu.vram[0][0x10] == 0xAB, "VRAM must round-trip");
    CHECK(gb->mmu.ppu.scx == 0x21, "PPU registers must round-trip");

    remove(path);
}

int main(void)
{
    static GB gb;

    CHECK(gb_init(&gb, false), "gb_init must succeed");
    printf("sizeof(GB) = %zu bytes\n", sizeof(GB));

    test_flag_masking();
    test_pair_aliasing();

    gb_reset(&gb);
    test_alu_flags(&gb);

    gb_reset(&gb);
    test_cycle_counts(&gb);

    gb_reset(&gb);
    test_stack_and_pop_af(&gb);

    gb_reset(&gb);
    test_interrupts(&gb);

    gb_reset(&gb);
    test_halt_bug(&gb);

    gb_reset(&gb);
    test_ei_delay(&gb);

    gb_reset(&gb);
    test_timer_edges(&gb);

    gb_reset(&gb);
    test_joypad_matrix(&gb);

    gb_reset(&gb);
    test_open_bus_and_echo(&gb);

    gb_reset(&gb);
    test_serial(&gb);

    gb_reset(&gb);
    test_oam_dma(&gb);

    gb_reset(&gb);
    test_save_state_round_trip(&gb);

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all core checks passed\n");
    return 0;
}
