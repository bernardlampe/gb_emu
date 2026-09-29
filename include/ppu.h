/* ppu.h — the LCD controller: registers, the mode machine, and the
 * scanline renderer (L19-L21).
 *
 * The PPU is a state machine clocked in T-cycles, and the framebuffer is its
 * OUTPUT, not its model. Drawing a scanline is a pure function of VRAM, OAM and
 * the registers at that moment; storing no per-pixel state is what makes partial
 * frame rendering possible.
 */
#ifndef GB_PPU_H
#define GB_PPU_H

#include "gbdefs.h"

typedef struct MMU MMU;

/* LCDC (FF40) bits. */
enum {
    LCDC_BG_ENABLE   = 0x01,  /* BG and window master enable */
    LCDC_OBJ_ENABLE  = 0x02,  /* sprite enable */
    LCDC_OBJ_HEIGHT  = 0x04,  /* 0 = 8x8, 1 = 8x16 */
    LCDC_BG_MAP      = 0x08,  /* 0 = 9800, 1 = 9C00 */
    LCDC_TILE_DATA   = 0x10,  /* 0 = 8800 signed, 1 = 8000 unsigned */
    LCDC_WIN_ENABLE  = 0x20,  /* window enable */
    LCDC_WIN_MAP     = 0x40,  /* 0 = 9800, 1 = 9C00 */
    LCDC_LCD_ENABLE  = 0x80   /* LCD master enable */
};

/* STAT (FF41) bits. */
enum {
    STAT_MODE        = 0x03,  /* 0 HBlank, 1 VBlank, 2 OAM scan, 3 drawing */
    STAT_LYC_EQ      = 0x04,  /* LY == LYC, read-only */
    STAT_MODE0_IRQ   = 0x08,
    STAT_MODE1_IRQ   = 0x10,
    STAT_MODE2_IRQ   = 0x20,
    STAT_LYC_IRQ     = 0x40
};

/* PPU modes. */
enum { PPU_HBLANK = 0, PPU_VBLANK = 1, PPU_OAM_SCAN = 2, PPU_DRAWING = 3 };

/* Scanline geometry, in dots (one dot is one T-cycle). */
#define DOTS_PER_LINE 456
#define OAM_SCAN_DOTS 80
#define DRAWING_DOTS_MIN 172
#define VISIBLE_LINES 144
#define TOTAL_LINES 154

/* DMG shades used when CGB palettes are absent: white, light, dark, black. */
#define DMG_SHADE_0 0x00E0F8D0u
#define DMG_SHADE_1 0x0088C070u
#define DMG_SHADE_2 0x00346856u
#define DMG_SHADE_3 0x00081820u

typedef struct {
    uint8_t lcdc, stat, scy, scx, ly, lyc;
    uint8_t bgp, obp0, obp1, wy, wx;

    int  dot;         /* dot within the current scanline, 0-455 */
    int  mode;        /* PPU_HBLANK / PPU_VBLANK / PPU_OAM_SCAN / PPU_DRAWING */
    int  win_line;    /* window internal line counter, 0-143 */
    bool frame_ready; /* set when line 153 ends: gb_run_frame() consumes it */
    bool stat_line;   /* previous STAT interrupt condition, for edge detect */
    bool cgb;         /* colour mode enabled */

    /* CGB palettes: 8 BG and 8 OBJ palettes of 4 colours, expanded to RGB. */
    uint8_t  bgpi, obpi;         /* FF68 / FF6A index registers */
    uint8_t  bgpd[64], obpd[64]; /* FF69 / FF6B data, 2 bytes per colour */
    uint32_t bg_pal[64], obj_pal[64];

    uint32_t fb[SCREEN_H][SCREEN_W];  /* XRGB8888 output, host reads it */
} PPU;

void ppu_init(PPU *p);
void ppu_reset(PPU *p);
void ppu_set_cgb(PPU *p, bool cgb);

/* Register access for FF40-FF4B, called from mmu_io_read/write. */
uint8_t ppu_read_reg(PPU *p, MMU *m, uint8_t reg);
void    ppu_write_reg(PPU *p, MMU *m, uint8_t reg, uint8_t v);

/* ppu_tick — advance the PPU by t_cycles T-cycles, driving the mode machine,
 * LY, LY=LYC coincidence, VBlank/STAT interrupts and scanline rendering. */
void ppu_tick(PPU *p, MMU *m, uint32_t t_cycles);

/* ppu_render_scanline — draw one visible line into the framebuffer. */
void ppu_render_scanline(PPU *p, MMU *m, int ly);

void ppu_serialize(PPU *p, FILE *f);
void ppu_deserialize(PPU *p, FILE *f);

#endif /* GB_PPU_H */
