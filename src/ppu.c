/* ppu.c — the LCD controller: registers, the mode machine, and the scanline
 * renderer (L19-L21).
 *
 * The PPU is a state machine clocked in T-cycles, and the framebuffer is its
 * OUTPUT, not its model. Drawing a scanline is a pure function of VRAM, OAM and the
 * registers *at that moment*; no per-pixel state survives the call. That is what
 * makes partial-frame rendering possible later (L28).
 *
 * Every mode transition is observable by the CPU through STAT, and games poll STAT
 * to time their VRAM writes, so the dot boundaries below are the contract, not an
 * implementation detail.
 *
 * Reference implementation for CS 4XX "System Emulation".
 */
#include "ppu.h"

#include <string.h>

#include "mmu.h"

/* DMG shades, indexed by the 2-bit colour index a palette entry produces. */
static const uint32_t DMG_SHADES[4] = {
    DMG_SHADE_0, DMG_SHADE_1, DMG_SHADE_2, DMG_SHADE_3
};

/* The sprites selected for the line being drawn.
 *
 * OAM holds 40 sprites but only 10 may appear on a line; the selection happens
 * once per line (mode 2), and mode 3's length depends on how many were found.
 * The list is file-scope because ppu_render_scanline() needs it and the header
 * deliberately exposes no per-line scratch space. */
typedef struct {
    uint8_t y, x, tile, attr;
} Sprite;

static Sprite line_sprites[10];
static int    line_sprite_count;

/* --- STAT helpers ------------------------------------------------------ */

/* ppu_update_lyc — STAT bit 2 mirrors (LY == LYC). The CPU cannot write it, so
 * the PPU is the only writer. */
static void ppu_update_lyc(PPU *p)
{
    if (p->ly == p->lyc) p->stat |= STAT_LYC_EQ;
    else                 p->stat &= (uint8_t)~STAT_LYC_EQ;
}

/* ppu_update_stat_irq — the STAT interrupt line.
 *
 * STAT is level-triggered on the OR of four conditions, but IF is raised only on
 * a rising edge of that OR: a game that polls STAT in a tight loop must not be
 * interrupted on every dot. p->stat_line remembers the previous level. */
static void ppu_update_stat_irq(PPU *p, MMU *m)
{
    bool line = ((p->mode == PPU_HBLANK   && (p->stat & STAT_MODE0_IRQ)) ||
                 (p->mode == PPU_VBLANK   && (p->stat & STAT_MODE1_IRQ)) ||
                 (p->mode == PPU_OAM_SCAN && (p->stat & STAT_MODE2_IRQ)) ||
                 (p->ly == p->lyc       && (p->stat & STAT_LYC_IRQ)));

    if (line && !p->stat_line) mmu_if_set(m, IF_STAT);
    p->stat_line = line;
}

/* ppu_set_mode — enter a new mode: mirror it into STAT's low two bits and
 * re-evaluate the interrupt line, because a mode change *is* a STAT event. */
static void ppu_set_mode(PPU *p, MMU *m, int mode)
{
    p->mode = mode;
    p->stat = (uint8_t)((p->stat & (uint8_t)~STAT_MODE) | (uint8_t)(mode & STAT_MODE));
    ppu_update_stat_irq(p, m);
}

/* --- CGB palette expansion --------------------------------------------- */

/* ppu_expand_colour — one 2-byte RGB555 colour to XRGB8888.
 *
 * Each 5-bit channel is expanded by replicating its top bits into the low ones,
 * (c << 3) | (c >> 2), so 31 becomes 255 and 0 stays 0. A plain shift would
 * make the brightest white slightly grey, which the CGB acid test notices. */
static uint32_t ppu_expand_colour(uint8_t lo, uint8_t hi)
{
    unsigned r5 = (unsigned)(lo & 0x1Fu);
    /* The green channel straddles the two bytes: 3 bits in the low byte, 2 in the
     * high one. */
    unsigned g5 = (((unsigned)lo >> 5) | ((unsigned)hi << 3)) & 0x1Fu;
    unsigned b5 = (unsigned)((hi >> 2) & 0x1Fu);

    unsigned r8 = (r5 << 3) | (r5 >> 2);
    unsigned g8 = (g5 << 3) | (g5 >> 2);
    unsigned b8 = (b5 << 3) | (b5 >> 2);

    return ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | (uint32_t)b8;
}

/* ppu_expand_entry — re-expand the one palette colour an index byte belongs to.
 * Palette index i occupies bytes 2i and 2i+1 (little-endian), so two writes per
 * colour and the palette is only consistent after the second. */
static void ppu_expand_entry(uint32_t *pal, const uint8_t *pd, unsigned index)
{
    unsigned entry = index >> 1;
    pal[entry] = ppu_expand_colour(pd[entry * 2], pd[entry * 2 + 1]);
}

/* ppu_expand_palettes — expand all 64 bytes of palette RAM. Called when the
 * machine's CGB flag is set, so the palettes are valid before the first write. */
static void ppu_expand_palettes(PPU *p)
{
    for (unsigned i = 0; i < 32; i++)
        ppu_expand_entry(p->bg_pal, p->bgpd, i * 2);
    for (unsigned i = 0; i < 32; i++)
        ppu_expand_entry(p->obj_pal, p->obpd, i * 2);
}

/* --- Sprite selection (mode 2) ----------------------------------------- */

/* ppu_scan_sprites — pick the sprites visible on the current line.
 *
 * DMG rule: walk OAM in order and keep the first 10 sprites whose vertical
 * range covers LY, then sort them by X ascending (a stable sort, so equal X keeps
 * OAM order). ppu_render_sprites draws that list backwards, which makes the
 * lowest-X sprite win each pixel and ties fall to the lower OAM index. */
static void ppu_scan_sprites(PPU *p, MMU *m)
{
    int height = (p->lcdc & LCDC_OBJ_HEIGHT) ? 16 : 8;

    line_sprite_count = 0;
    for (int i = 0; i < 40 && line_sprite_count < 10; i++) {
        uint16_t base = (uint16_t)(0xFE00 + i * 4);
        uint8_t sy = mmu_read8(m, base);

        /* OAM Y is the sprite's top edge minus 16, so Y=0 hides the sprite
         * above the screen without any special case. */
        int top = (int)sy - 16;
        if (p->ly < top || p->ly >= top + height) continue;

        line_sprites[line_sprite_count].y    = sy;
        line_sprites[line_sprite_count].x    = mmu_read8(m, (uint16_t)(base + 1));
        line_sprites[line_sprite_count].tile = mmu_read8(m, (uint16_t)(base + 2));
        line_sprites[line_sprite_count].attr = mmu_read8(m, (uint16_t)(base + 3));
        line_sprite_count++;
    }

    /* Insertion sort: at most ten entries, and stability is required for the
     * equal-X tie-break. */
    for (int i = 1; i < line_sprite_count; i++) {
        Sprite key = line_sprites[i];
        int j = i - 1;
        while (j >= 0 && line_sprites[j].x > key.x) {
            line_sprites[j + 1] = line_sprites[j];
            j--;
        }
        line_sprites[j + 1] = key;
    }
}

/* --- Scanline rendering ------------------------------------------------ */

/* ppu_bg_index — fetch one BG or window pixel's colour index.
 *
 * Everything here is pure address arithmetic on VRAM; there is no "current pixel"
 * state stored anywhere. The attribute byte is returned through attr_out because the
 * CGB needs it to choose the palette and the VRAM bank. */
static uint8_t ppu_bg_index(PPU *p, MMU *m, int x, int y, bool window,
                            uint8_t *attr_out)
{
    /* The window has its own map register; the BG map register only selects the
     * background's. */
    uint16_t map_base;
    if (window) map_base = (p->lcdc & LCDC_WIN_MAP) ? 0x9C00u : 0x9800u;
    else         map_base = (p->lcdc & LCDC_BG_MAP) ? 0x9C00u : 0x9800u;

    /* Window coordinates are window-relative; BG coordinates are scrolled. Both
     * wrap at 256, hence the narrowing to uint8_t. */
    uint8_t px = window ? (uint8_t)(x - ((int)p->wx - 7))
                        : (uint8_t)(x + p->scx);
    uint8_t py = window ? (uint8_t)p->win_line
                        : (uint8_t)(y + p->scy);

    /* One map byte per 8x8 tile, 32 tiles per map row. */
    uint16_t map_addr = (uint16_t)(map_base + (py / 8) * 32 + (px / 8));
    uint8_t index = mmu_read_vram(m, map_addr, 0);   /* tile number: bank 0 */

    /* CGB tile attributes live in bank 1 at the same address; attribute bit 3
     * picks the tile data bank. On DMG there is only bank 0. */
    uint8_t attr = p->cgb ? mmu_read_vram(m, map_addr, 1) : 0;
    *attr_out = attr;
    unsigned bank = (attr >> 3) & 1u;

    uint16_t tile_addr;
    if (p->lcdc & LCDC_TILE_DATA) {
        /* LCDC bit 4 set: unsigned indices based at 0x8000. */
        tile_addr = (uint16_t)(0x8000u + (unsigned)index * 16u);
    } else {
        /* LCDC bit 4 clear: signed indices relative to 0x9000, so 0x80 means
         * 0x8800. This is the addressing mode students always get wrong. */
        tile_addr = (uint16_t)(0x9000 + (int8_t)index * 16);
    }

    /* Two bytes per tile row: low bitplane first, then high. */
    unsigned row = (unsigned)(py % 8);
    uint8_t lo = mmu_read_vram(m, (uint16_t)(tile_addr + 2 * row), bank);
    uint8_t hi = mmu_read_vram(m, (uint16_t)(tile_addr + 2 * row + 1), bank);

    /* Bit 7 of each plane is the leftmost pixel. */
    unsigned bit = 7u - (unsigned)(px % 8);
    return (uint8_t)((((hi >> bit) & 1u) << 1) | ((lo >> bit) & 1u));
}

/* ppu_render_sprites — draw the sprites selected in mode 2.
 *
 * The list is walked backwards: the last sprite drawn for a pixel wins, and the
 * list was sorted so the highest-priority sprite (lowest X) comes last. */
static void ppu_render_sprites(PPU *p, MMU *m, int ly, const uint8_t *bg_index)
{
    int height = (p->lcdc & LCDC_OBJ_HEIGHT) ? 16 : 8;

    for (int s = line_sprite_count - 1; s >= 0; s--) {
        const Sprite *sp = &line_sprites[s];

        /* Row inside the sprite, 0 at its top edge. The list was built for this
         * LY, but a game may have changed LCDC's size bit since. */
        int row = ly - ((int)sp->y - 16);
        if (row < 0 || row >= height) continue;

        /* Attribute bit 6 mirrors the sprite vertically. */
        if (sp->attr & 0x40) row = height - 1 - row;

        unsigned tile = sp->tile;
        if (height == 16) {
            /* 8x16: bit 0 of the index is ignored and the two tiles are
             * (tile & 0xFE) and (tile | 1); the second is the lower half. */
            tile = (tile & 0xFEu) + (row >= 8 ? 1u : 0u);
            row %= 8;
        }

        /* Attribute bit 3 selects the sprite's tile data bank on CGB only. */
        unsigned bank = p->cgb ? ((unsigned)(sp->attr >> 3) & 1u) : 0u;
        uint16_t tile_addr =
            (uint16_t)(0x8000u + tile * 16u + (unsigned)row * 2u);
        uint8_t lo = mmu_read_vram(m, tile_addr, bank);
        uint8_t hi = mmu_read_vram(m, (uint16_t)(tile_addr + 1u), bank);

        for (int px = 0; px < 8; px++) {
            /* OAM X is offset by 8: X=0 puts the sprite just off the left. */
            int x = (int)sp->x - 8 + px;
            if (x < 0 || x >= SCREEN_W) continue;

            /* Attribute bit 5 mirrors the sprite horizontally. */
            unsigned bit = (sp->attr & 0x20) ? (unsigned)px : 7u - (unsigned)px;
            unsigned ci = (unsigned)((((hi >> bit) & 1u) << 1) | ((lo >> bit) & 1u));

            if (ci == 0) continue;   /* colour index 0 is transparent */

            /* Attribute bit 7 puts the sprite behind BG, but only where the BG
             * colour index is non-zero: over a blank BG it still shows. */
            if ((sp->attr & 0x80) && bg_index[x] != 0) continue;

            if (p->cgb) {
                /* CGB: attribute bits 0-2 pick one of the eight OBJ palettes,
                 * exactly as they pick one of the eight BG palettes. */
                unsigned pal = (unsigned)(sp->attr & 0x07u);
                p->fb[ly][x] = p->obj_pal[pal * 4u + ci];
            } else {
                /* OBP0/OBP1 hold four 2-bit shades, two bits per index. */
                uint8_t obp = (sp->attr & 0x10) ? p->obp1 : p->obp0;
                unsigned shade = ((unsigned)obp >> (ci * 2u)) & 3u;
                p->fb[ly][x] = DMG_SHADES[shade];
            }
        }
    }
}

void ppu_render_scanline(PPU *p, MMU *m, int ly)
{
    if (ly < 0 || ly >= VISIBLE_LINES) return;   /* nothing to draw there */

    bool bg_enabled = (p->lcdc & LCDC_BG_ENABLE) != 0;

    /* The window's left edge is WX-7 and it is drawn from WY down; WX > 166 and
     * WY > 143 are off-screen. The window is part of the BG layer, so it needs
     * LCDC bit 0 too. */
    bool window_on = bg_enabled && (p->lcdc & LCDC_WIN_ENABLE) &&
                     ly >= (int)p->wy && p->wx <= 166 && p->wy <= 143;
    int win_x = (int)p->wx - 7;

    /* The BG colour index of every pixel is kept: the sprite pass needs it for
     * the per-pixel BG-over-OBJ priority rule. */
    uint8_t bg_index[SCREEN_W];

    for (int x = 0; x < SCREEN_W; x++) {
        uint32_t colour;

        if (!bg_enabled) {
            /* LCDC bit 0 clear: the BG and window are blanked to white, and
             * their colour index is 0, so every sprite wins priority. */
            bg_index[x] = 0;
            colour = DMG_SHADE_0;
        } else {
            bool in_window = window_on && x >= win_x;
            uint8_t attr = 0;
            uint8_t ci = ppu_bg_index(p, m, x, ly, in_window, &attr);
            bg_index[x] = ci;

            if (p->cgb) {
                /* CGB: attribute bits 0-2 pick one of the eight BG palettes,
                 * each of which holds four RGB888 colours. */
                unsigned pal = (unsigned)(attr & 0x07u);
                colour = p->bg_pal[pal * 4u + ci];
            } else {
                /* DMG: BGP holds four 2-bit shades, two bits per index. */
                unsigned shade = ((unsigned)p->bgp >> (ci * 2u)) & 3u;
                colour = DMG_SHADES[shade];
            }
        }

        p->fb[ly][x] = colour;
    }

    /* The window's internal line counter advances on lines where the window is
     * on, and resets while it is off. A game that turns the window off and on
     * mid-frame (a HUD that appears partway down) relies on the reset: without
     * it the window starts on the wrong tile row. */
    if (window_on) p->win_line++;
    else           p->win_line = 0;

    if (p->lcdc & LCDC_OBJ_ENABLE)
        ppu_render_sprites(p, m, ly, bg_index);
}

/* --- Public interface -------------------------------------------------- */

void ppu_init(PPU *p)
{
    memset(p, 0, sizeof(*p));

    /* Until a CGB palette is written, every entry is shade 0 (white), so a game
     * that never touches FF69/FF6B still sees a defined picture. */
    for (int i = 0; i < 64; i++) {
        p->bg_pal[i] = DMG_SHADE_0;
        p->obj_pal[i] = DMG_SHADE_0;
    }
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++)
            p->fb[y][x] = DMG_SHADE_0;

    p->bgp = 0xFCu;    /* all four BG indices map to shade 0 */
    p->obp0 = 0xFFu;  /* both OBJ palettes map every index to shade 3 */
    p->obp1 = 0xFFu;
    p->mode = PPU_HBLANK;
}

void ppu_reset(PPU *p)
{
    /* Post-boot-ROM values from Pan Docs: the state the boot ROM leaves behind,
     * so a ROM that skips the boot ROM sees what a real machine would. */
    p->lcdc = 0x91u;   /* LCD on, BG on, OBJ on, tile data at 0x8000 */
    p->stat = 0x85u;    /* LY=LYC set; the mode bits are the PPU's */
    p->scy = 0;
    p->scx = 0;
    p->ly = 0;
    p->lyc = 0;
    p->wy = 0;
    p->wx = 0;
    p->bgp = 0xFCu;
    p->obp0 = 0xFFu;
    p->obp1 = 0xFFu;

    p->dot = 0;
    p->mode = PPU_OAM_SCAN;
    p->win_line = 0;
    p->frame_ready = false;
    p->stat_line = false;

    /* The palette RAM and the expanded palettes are deliberately kept: they are
     * not part of the DMG register file, and a reset must not desynchronise the
     * two representations of the same colour. p->cgb is set by ppu_set_cgb(). */
}

void ppu_set_cgb(PPU *p, bool cgb)
{
    p->cgb = cgb;
    /* A CGB game reads palettes before writing them, so expand whatever the 64
     * bytes of palette RAM currently hold. */
    ppu_expand_palettes(p);
}

uint8_t ppu_read_reg(PPU *p, MMU *m, uint8_t reg)
{
    (void)m;   /* every register here is PPU-owned state, not bus state */

    switch (reg) {
    case 0x40: return p->lcdc;
    case 0x41: return (uint8_t)(p->stat | 0x80u);   /* bit 7 reads as 1 */
    case 0x42: return p->scy;
    case 0x43: return p->scx;
    case 0x44: return p->ly;    /* LY is read-only to the CPU */
    case 0x45: return p->lyc;
    case 0x47: return p->bgp;
    case 0x48: return p->obp0;
    case 0x49: return p->obp1;
    case 0x4A: return p->wy;
    case 0x4B: return p->wx;
    case 0x68: return p->bgpi;
    case 0x69: return p->bgpd[p->bgpi & 0x3Fu];
    case 0x6A: return p->obpi;
    case 0x6B: return p->obpd[p->obpi & 0x3Fu];
    default:   return 0xFFu;   /* not a PPU register */
    }
}

void ppu_write_reg(PPU *p, MMU *m, uint8_t reg, uint8_t v)
{
    switch (reg) {
    case 0x40: {
        bool was_on = (p->lcdc & LCDC_LCD_ENABLE) != 0;
        bool now_on = (v & LCDC_LCD_ENABLE) != 0;
        p->lcdc = v;

        if (now_on && !was_on) {
            /* Turning the LCD on restarts the frame at line 0 in mode 2 with a
             * fresh window counter, and LY=LYC is re-evaluated against LY=0. */
            p->ly = 0;
            p->dot = 0;
            p->win_line = 0;
            ppu_update_lyc(p);
            ppu_set_mode(p, m, PPU_OAM_SCAN);
            p->frame_ready = false;
        } else if (!now_on && was_on) {
            /* Turning the LCD off freezes the PPU at line 0 in mode 0. */
            p->ly = 0;
            p->dot = 0;
            ppu_update_lyc(p);
            ppu_set_mode(p, m, PPU_HBLANK);
        }
        break;
    }

    case 0x41:
        /* Only bits 3-6 are writable; the mode bits and LY=LYC belong to the
         * PPU. Enabling a condition that is already true is a rising edge, so
         * the interrupt line is re-evaluated here too. */
        p->stat = (uint8_t)((p->stat & 0x07u) | (v & 0x78u));
        ppu_update_stat_irq(p, m);
        break;

    case 0x42: p->scy = v; break;
    case 0x43: p->scx = v; break;
    case 0x44: break;   /* LY is read-only: writes are ignored */
    case 0x45:
        p->lyc = v;
        /* LYC is compared against the *current* LY immediately. */
        ppu_update_lyc(p);
        ppu_update_stat_irq(p, m);
        break;
    case 0x47: p->bgp = v; break;
    case 0x48: p->obp0 = v; break;
    case 0x49: p->obp1 = v; break;
    case 0x4A: p->wy = v; break;
    case 0x4B: p->wx = v; break;

    case 0x68:
        p->bgpi = v;   /* bit 7 auto-increment, bits 0-5 index */
        break;
    case 0x69:
        p->bgpd[p->bgpi & 0x3Fu] = v;
        /* The colour only becomes visible after its second byte, so re-expand
         * the entry the write touched. */
        ppu_expand_entry(p->bg_pal, p->bgpd, (unsigned)(p->bgpi & 0x3Fu));
        if (p->bgpi & 0x80u)
            p->bgpi = (uint8_t)((p->bgpi & 0x80u) | ((p->bgpi + 1u) & 0x3Fu));
        break;

    case 0x6A:
        p->obpi = v;
        break;
    case 0x6B:
        p->obpd[p->obpi & 0x3Fu] = v;
        ppu_expand_entry(p->obj_pal, p->obpd, (unsigned)(p->obpi & 0x3Fu));
        if (p->obpi & 0x80u)
            p->obpi = (uint8_t)((p->obpi & 0x80u) | ((p->obpi + 1u) & 0x3Fu));
        break;

    default:
        break;   /* not a PPU register */
    }
}

void ppu_tick(PPU *p, MMU *m, uint32_t t_cycles)
{
    if (!(p->lcdc & LCDC_LCD_ENABLE)) {
        /* LCD off: LY is held at 0 in mode 0 and the PPU consumes no time.
         * No STAT interrupt is raised here, or a game waiting for VBlank while
         * the LCD is off would see an interrupt storm and never get one. */
        p->dot = 0;
        p->ly = 0;
        p->mode = PPU_HBLANK;
        p->stat = (uint8_t)(p->stat & (uint8_t)~STAT_MODE);
        p->stat_line = false;
        return;
    }

    /* One dot per T-cycle: a single CPU instruction can straddle a mode
     * boundary, and games time their VRAM writes against exactly that. */
    for (uint32_t i = 0; i < t_cycles; i++) {
        switch (p->mode) {
        case PPU_OAM_SCAN:
            if (p->dot == 0) {
                /* Line start: select this line's sprites, then draw. Drawing
                 * happens here (mode 2) so the pixels are ready before the CPU
                 * may write VRAM in mode 0, and so mode 3's length is known. */
                ppu_scan_sprites(p, m);
                if (p->ly < VISIBLE_LINES) ppu_render_scanline(p, m, p->ly);
            }
            p->dot++;
            if (p->dot >= OAM_SCAN_DOTS) ppu_set_mode(p, m, PPU_DRAWING);
            break;

        case PPU_DRAWING: {
            /* Each sprite on the line costs six extra dots. The clamp is a
             * guard: the nominal maximum is 172 + 6*10 = 232. */
            int length = DRAWING_DOTS_MIN + 6 * line_sprite_count;
            if (length > 289) length = 289;

            p->dot++;
            if (p->dot >= OAM_SCAN_DOTS + length) {
                ppu_set_mode(p, m, PPU_HBLANK);
                /* CGB HBlank DMA transfers 16 bytes per HBlank. */
                mmu_hdma_hblank(m);
            }
            break;
        }

        case PPU_HBLANK:
            p->dot++;

            /* LY=153 quirk: that line begins with 4 dots of mode 0 before
             * VBlank resumes, so the mode-1 STAT condition becomes true 4 dots
             * into the line rather than at its start. */
            if (p->ly == TOTAL_LINES - 1 && p->dot >= 4) {
                ppu_set_mode(p, m, PPU_VBLANK);
                break;
            }

            if (p->dot >= DOTS_PER_LINE) {
                p->dot = 0;
                p->ly++;
                ppu_update_lyc(p);   /* STAT bit 2 = (LY == LYC) */

                if (p->ly == VISIBLE_LINES) {
                    /* Line 144: VBlank begins and the window counter is
                     * cleared for the next frame. */
                    p->win_line = 0;
                    ppu_set_mode(p, m, PPU_VBLANK);
                    mmu_if_set(m, IF_VBLANK);
                } else if (p->ly == TOTAL_LINES - 1) {
                    /* Line 153: the quirk above gives it 4 dots of mode 0. */
                    ppu_set_mode(p, m, PPU_HBLANK);
                    mmu_hdma_hblank(m);
                } else {
                    ppu_set_mode(p, m, PPU_OAM_SCAN);
                }
            }
            break;

        case PPU_VBLANK:
            p->dot++;
            if (p->dot >= DOTS_PER_LINE) {
                p->dot = 0;
                if (p->ly == TOTAL_LINES - 1) {
                    /* Line 153 ends: the frame is complete, so gb_run_frame()
                     * can stop, and line 0 starts again. */
                    p->ly = 0;
                    ppu_update_lyc(p);
                    ppu_set_mode(p, m, PPU_OAM_SCAN);
                    p->frame_ready = true;
                } else {
                    p->ly++;
                    ppu_update_lyc(p);
                    if (p->ly == TOTAL_LINES - 1) {
                        ppu_set_mode(p, m, PPU_HBLANK);
                        mmu_hdma_hblank(m);
                    } else {
                        ppu_set_mode(p, m, PPU_VBLANK);
                    }
                }
            }
            break;

        default:
            /* Unreachable: p->mode is always one of the four above. */
            ppu_set_mode(p, m, PPU_OAM_SCAN);
            break;
        }
    }
}

void ppu_serialize(PPU *p, FILE *f)
{
    GB_SER(p->lcdc, f);
    GB_SER(p->stat, f);
    GB_SER(p->scy, f);
    GB_SER(p->scx, f);
    GB_SER(p->ly, f);
    GB_SER(p->lyc, f);
    GB_SER(p->bgp, f);
    GB_SER(p->obp0, f);
    GB_SER(p->obp1, f);
    GB_SER(p->wy, f);
    GB_SER(p->wx, f);

    GB_SER(p->dot, f);
    GB_SER(p->mode, f);
    GB_SER(p->win_line, f);
    GB_SER(p->frame_ready, f);
    GB_SER(p->stat_line, f);
    GB_SER(p->cgb, f);

    GB_SER(p->bgpi, f);
    GB_SER(p->obpi, f);
    GB_SER(p->bgpd, f);
    GB_SER(p->obpd, f);
    GB_SER(p->bg_pal, f);
    GB_SER(p->obj_pal, f);

    /* The sprites selected for the current line are state: a save taken in the
     * middle of mode 3 must resume with the same line length. */
    GB_SER(line_sprite_count, f);
    GB_SER(line_sprites, f);

    /* The framebuffer is an output, not state: it is regenerated on load. */
}

void ppu_deserialize(PPU *p, FILE *f)
{
    GB_DESER(p->lcdc, f);
    GB_DESER(p->stat, f);
    GB_DESER(p->scy, f);
    GB_DESER(p->scx, f);
    GB_DESER(p->ly, f);
    GB_DESER(p->lyc, f);
    GB_DESER(p->bgp, f);
    GB_DESER(p->obp0, f);
    GB_DESER(p->obp1, f);
    GB_DESER(p->wy, f);
    GB_DESER(p->wx, f);

    GB_DESER(p->dot, f);
    GB_DESER(p->mode, f);
    GB_DESER(p->win_line, f);
    GB_DESER(p->frame_ready, f);
    GB_DESER(p->stat_line, f);
    GB_DESER(p->cgb, f);

    GB_DESER(p->bgpi, f);
    GB_DESER(p->obpi, f);
    GB_DESER(p->bgpd, f);
    GB_DESER(p->obpd, f);
    GB_DESER(p->bg_pal, f);
    GB_DESER(p->obj_pal, f);

    GB_DESER(line_sprite_count, f);
    GB_DESER(line_sprites, f);

    /* The framebuffer was not saved: fill it with a defined colour so the
     * host never blits uninitialised memory, and let the next completed frame
     * overwrite it. */
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++)
            p->fb[y][x] = DMG_SHADE_0;
}
