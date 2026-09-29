/* gb.h — the machine: one CPU on one bus, plus the host-facing API.
 *
 * This is the only header the host (main.c) needs. The host owns a GB on the
 * heap, feeds it a ROM, runs frames, drains audio, and reads the framebuffer.
 * It never reaches into a subsystem directly.
 */
#ifndef GB_H
#define GB_H

#include "cpu.h"
#include "debug.h"
#include "gbdefs.h"
#include "mmu.h"

typedef struct GB {
    CPU   cpu;
    MMU   mmu;
    Debug debug;

    uint64_t frames;         /* frames completed since reset */
    uint64_t frame_overruns; /* frames the PPU failed to finish in two frames */
    uint64_t speed_rem;      /* CGB double-speed cycle remainder */
    bool     cgb;             /* CGB hardware model selected */
} GB;

/* --- Lifecycle --------------------------------------------------------- */
bool gb_init(GB *gb, bool cgb);
void gb_free(GB *gb);
void gb_reset(GB *gb);

/* gb_load_rom — copy the ROM, parse the header, select the mapper, and set
 * the CPU to the post-boot state (or to PC=0 if a boot ROM is installed). */
bool gb_load_rom(GB *gb, const uint8_t *rom, size_t size, const char *rom_path);
void gb_set_boot_rom(GB *gb, const uint8_t *rom, size_t size);

/* gb_run_frame — run exactly one video frame (70224 T-cycles). */
void gb_run_frame(GB *gb);
/* gb_run_instruction — one CPU instruction; the debugger uses this. */
uint32_t gb_run_instruction(GB *gb);

/* --- Host-facing accessors ---------------------------------------------- */
const uint32_t *gb_framebuffer(const GB *gb);   /* SCREEN_H * SCREEN_W XRGB */
bool  gb_frame_ready(const GB *gb);
size_t gb_drain_audio(GB *gb, int16_t *out, size_t max_frames);
const char *gb_serial_output(const GB *gb, size_t *len);
void  gb_clear_serial(GB *gb);
void  gb_set_link(GB *gb, FILE *in, FILE *out);
uint64_t gb_frames_run(const GB *gb);

/* --- Input ------------------------------------------------------------- */
void gb_set_buttons(GB *gb, uint8_t buttons, uint8_t dpad);

/* --- Save states (L29) ------------------------------------------------- */
bool gb_save_state(GB *gb, const char *path);
bool gb_load_state(GB *gb, const char *path);

#endif /* GB_H */
