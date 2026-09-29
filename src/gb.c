/* gb.c — the machine: one CPU on one bus, plus the host-facing API.
 *
 * This file owns the frame loop and the save-state format. It is the only place
 * that knows how the parts fit together, which keeps every module testable on its
 * own and keeps the host (main.c) free of emulation logic.
 *
 * Reference implementation for CS 4XX "System Emulation".
 */
#include "gb.h"

#include <stdarg.h>
#include <string.h>

/* ======================================================================
 * 1. Logging — the core's only output channel
 * ====================================================================== */

static void gb_log_default(const char *line)
{
    fputs(line, stderr);
    fputc('\n', stderr);
}

static gb_log_fn g_log_sink = gb_log_default;

void gb_log_set_sink(gb_log_fn sink)
{
    g_log_sink = sink ? sink : gb_log_default;
}

void gb_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    g_log_sink(buf);
}

/* ======================================================================
 * 2. Lifecycle
 * ====================================================================== */

bool gb_init(GB *gb, bool cgb)
{
    memset(gb, 0, sizeof(*gb));

    cpu_build_tables();
    mmu_init(&gb->mmu, cgb);
    debug_init(&gb->debug);
    gb->mmu.debug = &gb->debug;    /* watchpoints reach the bus this way */

    gb->cgb = cgb;
    gb->frames = 0;
    gb->speed_rem = 0;
    return true;
}

void gb_free(GB *gb)
{
    cart_free(&gb->mmu.cart);
}

void gb_reset(GB *gb)
{
    mmu_reset(&gb->mmu);
    cpu_reset(&gb->cpu, gb->cgb);
    debug_init(&gb->debug);
    gb->frames = 0;
    gb->frame_overruns = 0;
    gb->speed_rem = 0;
}

bool gb_load_rom(GB *gb, const uint8_t *rom, size_t size, const char *rom_path)
{
    gb_reset(gb);

    if (!cart_load(&gb->mmu.cart, rom, size, rom_path)) return false;

    /* A CGB-only cartridge on a DMG machine cannot run at all. Refusing with
     * a diagnostic beats rendering garbage. */
    if (gb->mmu.cart.header.cgb_only && !gb->cgb) {
        gb_log("cartridge requires CGB hardware; run with --cgb");
        return false;
    }

    gb_log("cart: '%s' type %02X mbc %d rom %zu bytes ram %zu bytes%s%s",
           gb->mmu.cart.header.title, gb->mmu.cart.header.cart_type,
           (int)gb->mmu.cart.mbc, gb->mmu.cart.rom_size, gb->mmu.cart.ram_size,
           gb->mmu.cart.has_battery ? " battery" : "",
           gb->mmu.cart.has_rtc ? " rtc" : "");

    /* Battery RAM is restored before the first frame, so a game sees its save
     * from the very first instruction. */
    if (gb->mmu.cart.has_battery) cart_battery_load(&gb->mmu.cart);

    /* Without a boot ROM the CPU starts at the post-boot state with PC=0100. */
    if (!gb->mmu.boot_rom_enabled) cpu_reset(&gb->cpu, gb->cgb);
    return true;
}

void gb_set_boot_rom(GB *gb, const uint8_t *rom, size_t size)
{
    mmu_set_boot_rom(&gb->mmu, rom, size);
    if (gb->mmu.boot_rom_enabled) {
        /* With a boot ROM installed the CPU starts at 0000 and the registers
         * start zeroed, exactly as the boot ROM expects. */
        memset(&gb->cpu, 0, sizeof(gb->cpu));
        gb->cpu.pc = 0x0000;
    }
}

void gb_set_link(GB *gb, FILE *in, FILE *out)
{
    mmu_set_link(&gb->mmu, in, out);
}

/* ======================================================================
 * 3. The frame loop
 *
 * One frame is 70224 T-cycles: 154 scanlines of 456 dots. The loop ticks
 * every subsystem with the SAME cycle count, so nothing can drift.
 * ====================================================================== */

uint32_t gb_run_instruction(GB *gb)
{
    MMU *m = &gb->mmu;

    debug_record(gb);                    /* L15: trace the instruction about to run */

    uint32_t cycles = cpu_step(&gb->cpu, m);

    /* In CGB double-speed mode the CPU runs twice as fast as the peripherals,
     * so they see half the cycles. The remainder accumulator keeps this exact
     * over time: no drift, and replays still match. */
    uint32_t periph = cycles;
    if (m->double_speed) {
        gb->speed_rem += cycles;
        periph = (uint32_t)(gb->speed_rem / 2u);
        gb->speed_rem -= (uint64_t)periph * 2u;
    }

    timer_tick(&m->timer, periph, m);
    ppu_tick(&m->ppu, m, periph);
    apu_tick(&m->apu, m, periph);
    mmu_tick(m, periph);
    cart_tick(&m->cart, periph);

    /* The boot ROM hands over by jumping to 0100 or by writing FF50. */
    if (m->boot_rom_enabled && gb->cpu.pc >= 0x100u) m->boot_rom_enabled = false;

    return cycles;
}

void gb_run_frame(GB *gb)
{
    gb->mmu.ppu.frame_ready = false;

    /* With the LCD off the PPU consumes no time and can never reach VBlank, so
     * the CPU clock defines the frame instead: 70224 T-cycles, the same length
     * VBlank would have given. This is the common boot case (a game clears
     * VRAM with the LCD off), and it keeps the frame's pacing correct. */
    if (!(gb->mmu.ppu.lcdc & LCDC_LCD_ENABLE)) {
        uint32_t cycles = 0u;
        while (cycles < FRAME_TICKS)
            cycles += gb_run_instruction(gb);
        gb->frames++;
        return;
    }

    /* With the LCD on, the PPU says when a frame is done. A game that turns
     * the LCD off (or restarts it) in the middle of a frame stops the PPU from
     * ever reaching VBlank, so that frame falls back to the CPU clock too. The
     * frame length must not depend on what the game does to LCDC, or the audio
     * the host produces per frame stops matching the frame it presents. */
    uint32_t cycles = 0u;

    while (cycles < FRAME_TICKS) {
        cycles += gb_run_instruction(gb);
        if (gb->mmu.ppu.frame_ready) break;
        if (!(gb->mmu.ppu.lcdc & LCDC_LCD_ENABLE)) {
            /* LCD off mid-frame: finish the frame on the CPU clock. */
            while (cycles < FRAME_TICKS)
                cycles += gb_run_instruction(gb);
            break;
        }
    }

    if (!gb->mmu.ppu.frame_ready && (gb->mmu.ppu.lcdc & LCDC_LCD_ENABLE)) {
        /* The LCD was on for the whole frame and the PPU still did not reach
         * VBlank: a real overrun, so say it once. The frame is dropped, never
         * the audio: dropping sound is audible, dropping a video frame is not. */
        if (gb->frame_overruns == 0u)
            gb_log("frame: the PPU did not finish a frame in one frame of CPU "
                   "time; check LCDC writes and the PPU's mode machine");
        gb->frame_overruns++;
    }

    gb->frames++;
}

/* ======================================================================
 * 4. Host-facing accessors
 * ====================================================================== */

const uint32_t *gb_framebuffer(const GB *gb)
{
    return &gb->mmu.ppu.fb[0][0];
}

bool gb_frame_ready(const GB *gb)
{
    return gb->mmu.ppu.frame_ready;
}

size_t gb_drain_audio(GB *gb, int16_t *out, size_t max_frames)
{
    return apu_drain(&gb->mmu.apu, out, max_frames);
}

const char *gb_serial_output(const GB *gb, size_t *len)
{
    if (len) *len = gb->mmu.serial_len;
    return gb->mmu.serial_buf;
}

void gb_clear_serial(GB *gb)
{
    gb->mmu.serial_len = 0;
    gb->mmu.serial_buf[0] = '\0';
}

uint64_t gb_frames_run(const GB *gb)
{
    return gb->frames;
}

void gb_set_buttons(GB *gb, uint8_t buttons, uint8_t dpad)
{
    joypad_set_input(&gb->mmu.joypad, buttons, dpad, &gb->mmu);
}

/* ======================================================================
 * 5. Save states (L29)
 *
 * The format is a magic number, a version, then each subsystem in a fixed
 * order. It is same-build, same-machine only, which the README states; a
 * portable format is the capstone stretch goal.
 * ====================================================================== */

#define GB_STATE_MAGIC   0x4742444Du   /* "GBDM" */
#define GB_STATE_VERSION 1u

bool gb_save_state(GB *gb, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        gb_log("cannot open save state '%s' for writing", path);
        return false;
    }

    uint32_t magic = GB_STATE_MAGIC;
    uint32_t version = GB_STATE_VERSION;

    GB_SER(magic, f);
    GB_SER(version, f);
    cpu_serialize(&gb->cpu, f);
    mmu_serialize(&gb->mmu, f);
    debug_serialize(&gb->debug, f);
    GB_SER(gb->frames, f);
    GB_SER(gb->speed_rem, f);
    GB_SER(gb->cgb, f);

    /* Battery RAM is a separate file: it must survive a reset, not just a
     * save-state load. */
    cart_battery_save(&gb->mmu.cart);

    fclose(f);
    return true;
}

bool gb_load_state(GB *gb, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        gb_log("cannot open save state '%s' for reading", path);
        return false;
    }

    uint32_t magic = 0, version = 0;
    GB_DESER(magic, f);
    GB_DESER(version, f);

    if (magic != GB_STATE_MAGIC || version != GB_STATE_VERSION) {
        gb_log("save state '%s' is not a version %u state file", path, GB_STATE_VERSION);
        fclose(f);
        return false;
    }

    cpu_deserialize(&gb->cpu, f);
    mmu_deserialize(&gb->mmu, f);
    debug_deserialize(&gb->debug, f);
    GB_DESER(gb->frames, f);
    GB_DESER(gb->speed_rem, f);
    GB_DESER(gb->cgb, f);

    fclose(f);
    return true;
}
