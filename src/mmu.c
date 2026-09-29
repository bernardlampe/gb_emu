/* mmu.c — address decoding, I/O dispatch, DMA, serial, and open bus (L3-L4).
 *
 * Everything in the machine reaches memory through mmu_read8()/mmu_write8().
 * Keeping one choke point is what makes it possible to add the OAM DMA lockout,
 * the open bus, wait states and debugger watchpoints without touching the CPU.
 *
 * Reference implementation for CS 4XX "System Emulation".
 */
#include "mmu.h"

#include <string.h>

/* ======================================================================
 * 1. Address decoding
 * ====================================================================== */

/* region_of — which device owns this address?
 *
 * Deliberately written as comparisons rather than a page table: 0xFE00-0xFEFF
 * is not page aligned (OAM is 160 bytes and the next 96 are unused), so a
 * 4 KiB table would be wrong at that boundary. This function is the memory map
 * from the course's first section, in code.
 */
static Region region_of(uint16_t addr)
{
    if (addr < 0x8000u) return (addr < 0x4000u) ? REGION_ROM0 : REGION_ROMX;
    if (addr < 0xA000u) return REGION_VRAM;    /* 8 KiB, 2 banks on CGB   */
    if (addr < 0xC000u) return REGION_SRAM;    /* cart RAM, MBC-controlled */
    if (addr < 0xE000u) return REGION_WRAM;    /* 8 KiB, 4 KiB banks on CGB */
    if (addr < 0xFE00u) return REGION_ECHO;    /* mirrors C000-DFFF         */
    if (addr < 0xFEA0u) return REGION_OAM;     /* 160 bytes of sprites      */
    if (addr < 0xFF00u) return REGION_UNUSED;  /* reads as open bus         */
    if (addr < 0xFF80u) return REGION_IO;      /* hardware registers        */
    if (addr < 0xFFFFu) return REGION_HRAM;    /* 127 bytes, always alive   */
    return REGION_IE;
}

/* wram_bank_of — which 4 KiB bank of WRAM owns this address?
 *
 * 0xC000-0xCFFF is always bank 0. 0xD000-0xDFFF is the SVBK bank (1-7) on
 * the CGB; the DMG has one 8 KiB WRAM and no bank register, so there it is
 * bank 1. The 8 KiB window is therefore banks 0-1 on the DMG and banks 0 and
 * SVBK on the CGB, and the bank index can never run past wram[].
 *
 * Indexing the whole window with the raw SVBK value instead is the classic
 * "off the end of wram[]" bug: SVBK=7 makes the window banks 7-8, and bank 8
 * is where oam[]/io[]/ie live. */
static unsigned wram_bank_of(const MMU *m, uint16_t addr)
{
    if (addr < 0xD000u) return 0u;
    return m->cgb ? m->wram_bank : 1u;
}

/* ======================================================================
 * 2. Interrupt flags
 * ====================================================================== */

void mmu_if_set(MMU *m, uint8_t bit)
{
    m->if_reg |= (uint8_t)(bit & 0x1Fu);
}

void mmu_if_clear(MMU *m, uint8_t bit)
{
    m->if_reg &= (uint8_t)~(bit & 0x1Fu);
}

/* ======================================================================
 * 3. The I/O register map (L4)
 *
 * Registers with behaviour are routed to their device; registers with no
 * behaviour are kept in io[] so the debugger can still show them. This switch
 * is the table students are asked to produce in L4, in code.
 * ====================================================================== */

static void hdma_copy_block(MMU *m);
static void mmu_hdma_write5(MMU *m, uint8_t v);

static uint8_t io_read(MMU *m, uint8_t reg)
{
    switch (reg) {
    case 0x00: return joypad_read(&m->joypad);
    case 0x01: return m->sb;
    case 0x02: return m->sc;

    case 0x04: return (uint8_t)(m->timer.div_counter >> 8);   /* DIV */
    case 0x05: return m->timer.tima;
    case 0x06: return m->timer.tma;
    case 0x07: return (uint8_t)(m->timer.tac | 0xF8u);       /* upper bits read 1 */

    case 0x0F: return (uint8_t)(m->if_reg | 0xE0u);          /* upper bits read 1 */

    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B:
        return ppu_read_reg(&m->ppu, m, reg);

    case 0x46: return 0xFF;   /* DMA source: write-only */

    case 0x4D: return m->io[0x4D];       /* KEY1: CGB speed switch */
    case 0x4F: return m->cgb ? m->vram_bank : 0xFF;
    case 0x50: return m->boot_rom_enabled ? 0x00u : 0xFFu;

    case 0x51: case 0x52: case 0x53: case 0x54:
        return 0xFF;                        /* HDMA source/dest: write-only */
    case 0x55: return m->hdma5;

    case 0x68: case 0x69: case 0x6A: case 0x6B:
        return ppu_read_reg(&m->ppu, m, reg);   /* CGB palettes */

    case 0x70: return m->cgb ? (uint8_t)(m->wram_bank | 0xF8u) : 0xFF;

    default:
        /* APU registers 0x10-0x3F are 0xFF when the APU is powered off. */
        if (reg >= 0x10u && reg <= 0x3Fu) return apu_read_reg(&m->apu, reg);
        return m->io[reg];
    }
}

static void io_write(MMU *m, uint8_t reg, uint8_t v)
{
    switch (reg) {
    case 0x00: joypad_write(&m->joypad, v); break;
    case 0x01: m->sb = v; break;
    case 0x02:
        m->sc = (uint8_t)(v & 0x81u);
        /* A write with bit 7 (start) and bit 0 (internal clock) set starts
         * an 8-bit transfer at 8192 Hz, i.e. 4096 T-cycles. Test ROMs write
         * 0x81 and poll bit 7 until it clears. */
        if ((v & 0x81u) == 0x81u) {
            m->serial_active = true;
            m->serial_cycles = 4096u;
            m->sc = 0x81u;
        }
        break;

    case 0x04: m->timer.div_counter = 0; break;   /* DIV resets the counter */
    case 0x05: m->timer.tima = v; break;
    case 0x06: m->timer.tma = v; break;
    case 0x07: m->timer.tac = (uint8_t)(v & 0x07u); break;

    case 0x0F: m->if_reg = (uint8_t)(v & 0x1Fu); break;

    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B:
        ppu_write_reg(&m->ppu, m, reg, v);
        break;

    case 0x46:
        /* OAM DMA: the value is the HIGH byte of the source address, which
         * is therefore aligned to 0xXX00. 160 bytes take 160 M-cycles. */
        m->dma_src = (uint16_t)(v << 8);
        m->dma_active = true;
        m->dma_pos = 0;
        m->dma_elapsed = 0;
        break;

    case 0x4D:
        /* KEY1 bit 0 arms a speed switch; STOP performs it (see cpu.c). */
        m->io[0x4D] = (uint8_t)(v & 1u);
        break;

    case 0x4F:
        if (m->cgb) m->vram_bank = (uint8_t)(v & 1u);
        break;

    case 0x50:
        /* Any write disables the boot ROM and unmaps 0000-00FF. */
        m->boot_rom_enabled = false;
        break;

    case 0x51: m->hdma_src = (uint16_t)((m->hdma_src & 0x00FFu) | (v << 8)); break;
    case 0x52: m->hdma_src = (uint16_t)((m->hdma_src & 0xFF00u) | (v & 0xF0u)); break;
    case 0x53: m->hdma_dst = (uint16_t)(((m->hdma_dst & 0x00FFu) | ((v & 0x1Fu) << 8)) | 0x8000u); break;
    case 0x54: m->hdma_dst = (uint16_t)((m->hdma_dst & 0xFF00u) | (v & 0xF0u)); break;
    case 0x55: mmu_hdma_write5(m, v); break;

    case 0x68: case 0x69: case 0x6A: case 0x6B:
        ppu_write_reg(&m->ppu, m, reg, v);
        break;

    case 0x70:
        /* SVBK value 0 selects bank 1: there is no bank 0 in WRAM. Only the
         * CGB banks its WRAM, so on the DMG the write is ignored and the 8 KiB
         * window stays at banks 0-1. */
        if (m->cgb)
            m->wram_bank = (v & 0x07u) ? (v & 0x07u) : 1u;
        break;

    default:
        if (reg >= 0x10u && reg <= 0x3Fu) apu_write_reg(&m->apu, reg, v);
        else m->io[reg] = v;   /* no behaviour, kept for the debug viewer */
        break;
    }
}

/* ======================================================================
 * 4. The bus
 * ====================================================================== */

uint8_t mmu_read8(MMU *m, uint16_t addr)
{
    if (m->debug) debug_watch(m->debug, addr, false);   /* L16 watchpoints */

    /* While an OAM DMA transfer is running, the CPU can only reach HRAM.
     * Games rely on this: they park a small routine in HRAM to prepare the
     * next frame while the transfer finishes. Mooneye's dma tests check it. */
    if (m->dma_active && addr < 0xFF80u) {
        m->open_bus = 0xFFu;
        return 0xFFu;
    }

    /* The boot ROM overlays the low page until it hands over. */
    if (m->boot_rom_enabled && addr < 0x100u) return m->boot_rom[addr];

    uint8_t v;
    switch (region_of(addr)) {
    case REGION_ROM0:
    case REGION_ROMX:
        v = cart_read_rom(&m->cart, addr);
        break;

    case REGION_VRAM:
        v = m->vram[m->vram_bank][addr - 0x8000u];
        break;

    case REGION_SRAM:
        v = cart_read_ram(&m->cart, addr);
        break;

    case REGION_WRAM:
        v = m->wram[wram_bank_of(m, addr)][addr & 0x0FFFu];
        break;

    case REGION_ECHO:
        /* Echo RAM is a mirror, not storage: redirecting instead of
         * duplicating is what keeps games that use it consistent. */
        v = mmu_read8(m, (uint16_t)(addr - 0x2000u));
        break;

    case REGION_OAM:
        v = m->oam[addr - 0xFE00u];
        break;

    case REGION_UNUSED:
        v = m->open_bus;    /* reads as the last value on the bus */
        break;

    case REGION_IO:
        v = io_read(m, (uint8_t)(addr & 0xFFu));
        break;

    case REGION_HRAM:
        v = m->hram[addr - 0xFF80u];
        break;

    default: /* REGION_IE */
        v = m->ie;
        break;
    }

    m->open_bus = v;
    return v;
}

void mmu_write8(MMU *m, uint16_t addr, uint8_t v)
{
    if (m->debug) debug_watch(m->debug, addr, true);   /* L16 watchpoints */

    m->open_bus = v;

    if (m->dma_active && addr < 0xFF80u) return;   /* HRAM only during DMA */

    switch (region_of(addr)) {
    case REGION_ROM0:
    case REGION_ROMX:
        cart_write_rom(&m->cart, addr, v);   /* bank registers, not ROM data */
        break;

    case REGION_VRAM:
        m->vram[m->vram_bank][addr - 0x8000u] = v;
        break;

    case REGION_SRAM:
        cart_write_ram(&m->cart, addr, v);
        break;

    case REGION_WRAM:
        m->wram[wram_bank_of(m, addr)][addr & 0x0FFFu] = v;
        break;

    case REGION_ECHO:
        mmu_write8(m, (uint16_t)(addr - 0x2000u), v);
        break;

    case REGION_OAM:
        m->oam[addr - 0xFE00u] = v;
        break;

    case REGION_UNUSED:
        break;   /* writes are ignored */

    case REGION_IO:
        io_write(m, (uint8_t)(addr & 0xFFu), v);
        break;

    case REGION_HRAM:
        m->hram[addr - 0xFF80u] = v;
        break;

    default: /* REGION_IE */
        m->ie = (uint8_t)(v & 0x1Fu);
        break;
    }
}

uint16_t mmu_read16(MMU *m, uint16_t addr)
{
    /* Little-endian: the low byte comes first, and each half updates the
     * open bus in turn, exactly as two 8-bit reads would. */
    uint16_t lo = mmu_read8(m, addr);
    uint16_t hi = mmu_read8(m, (uint16_t)(addr + 1u));
    return (uint16_t)(lo | (hi << 8));
}

void mmu_write16(MMU *m, uint16_t addr, uint16_t v)
{
    mmu_write8(m, addr, (uint8_t)(v & 0xFFu));
    mmu_write8(m, (uint16_t)(addr + 1u), (uint8_t)(v >> 8));
}

/* Banked VRAM access for the PPU: CGB tile attributes select a bank. */
uint8_t mmu_read_vram(MMU *m, uint16_t addr, unsigned bank)
{
    return m->vram[bank & 1u][addr - 0x8000u];
}

void mmu_write_vram(MMU *m, uint16_t addr, unsigned bank, uint8_t v)
{
    m->vram[bank & 1u][addr - 0x8000u] = v;
}

/* ======================================================================
 * 5. DMA
 * ====================================================================== */

/* dma_source_read — read from the DMA source without going through the
 * CPU-side bus, because the DMA engine is not blocked by itself. */
static uint8_t dma_source_read(MMU *m, uint16_t addr)
{
    if (addr < 0x8000u) return cart_read_rom(&m->cart, addr);
    if (addr < 0xA000u) return m->vram[m->vram_bank][addr - 0x8000u];
    if (addr < 0xC000u) return cart_read_ram(&m->cart, addr);
    if (addr < 0xE000u) return m->wram[wram_bank_of(m, addr)][addr & 0x0FFFu];
    if (addr >= 0xE000u && addr < 0xFE00u) return dma_source_read(m, (uint16_t)(addr - 0x2000u));
    return 0xFFu;
}

static void hdma_copy_block(MMU *m)
{
    for (unsigned i = 0; i < 16u; i++) {
        uint8_t v = dma_source_read(m, m->hdma_src);
        m->vram[0][(m->hdma_dst - 0x8000u) & 0x1FFFu] = v;
        m->hdma_src++;
        m->hdma_dst++;
        if (m->hdma_dst > 0x9FFFu) m->hdma_dst = 0x8000u;
    }
}

/* mmu_hdma_write5 — HDMA5. Bit 7 set starts an HBlank DMA, clear starts a
 * general-purpose DMA (only legal while the LCD is off). */
static void mmu_hdma_write5(MMU *m, uint8_t v)
{
    if (!m->cgb) return;

    if (m->hdma_hblank) {
        /* Writing with bit 7 clear while an HBlank DMA is running stops it
         * and reports the blocks that are left. */
        m->hdma_hblank = false;
        m->hdma5 = (uint8_t)(0x80u | (v & 0x7Fu));
        return;
    }

    if (v & 0x80u) {
        m->hdma5 = (uint8_t)(v & 0x7Fu);
        m->hdma_hblank = true;
    } else {
        if (m->ppu.lcdc & LCDC_LCD_ENABLE) return;  /* not while the LCD is on */
        unsigned blocks = (unsigned)(v & 0x7Fu) + 1u;
        while (blocks--) hdma_copy_block(m);
        m->hdma5 = 0xFFu;
    }
}

/* mmu_hdma_hblank — the PPU calls this once per HBlank on CGB. */
void mmu_hdma_hblank(MMU *m)
{
    if (!m->hdma_hblank) return;

    hdma_copy_block(m);
    if ((m->hdma5 & 0x7Fu) == 0u) {
        m->hdma_hblank = false;
        m->hdma5 = 0xFFu;    /* 0xFF reports "no transfer running" */
    } else {
        m->hdma5--;
    }
}

/* ======================================================================
 * 6. Ticking: DMA, HDMA and serial
 * ====================================================================== */

void mmu_tick(MMU *m, uint32_t t_cycles)
{
    /* OAM DMA: one byte per 4 T-cycles, 160 bytes in 160 M-cycles. Copying
     * progressively (rather than all at once at the end) is what makes a game
     * that reads OAM mid-transfer see what hardware would show it. */
    if (m->dma_active) {
        m->dma_elapsed += t_cycles;

        uint32_t want = m->dma_elapsed / 4u;
        if (want > OAM_SIZE) want = OAM_SIZE;

        while (m->dma_pos < want) {
            m->oam[m->dma_pos] =
                dma_source_read(m, (uint16_t)(m->dma_src + m->dma_pos));
            m->dma_pos++;
        }

        if (m->dma_pos >= OAM_SIZE) {
            m->dma_active = false;
            m->dma_elapsed = 0;
            m->dma_pos = 0;
        }
    }

    /* Serial: 8 bits at 512 Hz each. When the transfer completes, bit 7 of SC
     * clears and the serial interrupt is requested. */
    if (m->serial_active) {
        if (t_cycles >= m->serial_cycles) {
            m->serial_cycles = 0;
            m->serial_active = false;

            uint8_t sent = m->sb;
            m->sb = 0xFFu;                    /* no peer: the line idles high */
            if (m->link_in) {
                int c = fgetc(m->link_in);
                if (c != EOF) m->sb = (uint8_t)c;
            }
            if (m->link_out) { fputc(sent, m->link_out); fflush(m->link_out); }
            m->sc = 0x01u;                     /* transfer finished */
            if (m->serial_len + 1u < sizeof(m->serial_buf)) {
                m->serial_buf[m->serial_len++] = (char)sent;
                m->serial_buf[m->serial_len] = '\0';
            }
            mmu_if_set(m, IF_SERIAL);
        } else {
            m->serial_cycles -= t_cycles;
        }
    }
}

/* ======================================================================
 * 7. Reset, boot ROM, and save states
 * ====================================================================== */

void mmu_set_boot_rom(MMU *m, const uint8_t *rom, size_t size)
{
    m->boot_rom = rom;
    m->boot_rom_size = size;
    m->boot_rom_enabled = (rom != NULL && size >= 0x100u);
}

void mmu_set_link(MMU *m, FILE *in, FILE *out)
{
    m->link_in = in;
    m->link_out = out;
}

void mmu_reset(MMU *m)
{
    memset(m->vram, 0, sizeof(m->vram));
    memset(m->wram, 0, sizeof(m->wram));
    memset(m->oam, 0, sizeof(m->oam));
    memset(m->hram, 0, sizeof(m->hram));
    memset(m->io, 0, sizeof(m->io));

    m->ie = 0;
    m->if_reg = 0xE1u;      /* VBlank is pending on hardware at power-on */
    m->open_bus = 0xFFu;
    m->vram_bank = 0;
    m->wram_bank = 1;
    m->double_speed = false;

    m->dma_active = false;
    m->dma_pos = 0;
    m->dma_elapsed = 0;
    m->dma_src = 0;

    m->hdma_src = 0; m->hdma_dst = 0; m->hdma5 = 0xFFu; m->hdma_hblank = false;

    m->sb = 0; m->sc = 0x7Eu; m->serial_cycles = 0; m->serial_active = false;
    m->serial_len = 0; m->serial_buf[0] = '\0';

    m->t_cycles = 0;

    joypad_init(&m->joypad);
    timer_init(&m->timer);
    ppu_reset(&m->ppu);
    apu_reset(&m->apu);

    /* Post-boot register values, straight from Pan Docs: these are the values
     * the boot ROM leaves behind, so a ROM that skips the boot ROM sees the
     * same machine state a real one would. Registers owned by a device are set
     * by that device's reset (ppu_reset, apu_reset); only the free-standing
     * ones are set here. */
    m->io[0x00] = 0xCFu;   /* P1   */
    m->io[0x02] = 0x7Eu;   /* SC   */
    m->io[0x4D] = m->cgb ? 0x7Eu : 0xFFu;   /* KEY1 */
    m->io[0x4F] = 0xFEu;   /* VBK  */
    m->io[0x50] = 0xFFu;   /* BOOT */
    m->io[0x70] = 0xF8u;   /* SVBK */

    /* DIV reads 0x18 out of the boot ROM, i.e. the counter is at 0x1800. */
    m->timer.div_counter = 0x1800u;
}

void mmu_init(MMU *m, bool cgb)
{
    memset(m, 0, sizeof(*m));
    m->cgb = cgb;
    m->vram_bank = 0;
    m->wram_bank = 1;
    m->open_bus = 0xFFu;
    m->boot_rom = NULL;
    m->boot_rom_size = 0;
    m->boot_rom_enabled = false;
    m->link_in = NULL;
    m->link_out = NULL;

    joypad_init(&m->joypad);
    timer_init(&m->timer);
    ppu_init(&m->ppu);
    ppu_set_cgb(&m->ppu, cgb);
    apu_init(&m->apu);
}

void mmu_serialize(MMU *m, FILE *f)
{
    GB_SER(m->vram, f);
    GB_SER(m->wram, f);
    GB_SER(m->oam, f);
    GB_SER(m->hram, f);
    GB_SER(m->io, f);
    GB_SER(m->ie, f); GB_SER(m->if_reg, f); GB_SER(m->open_bus, f);
    GB_SER(m->cgb, f); GB_SER(m->double_speed, f);
    GB_SER(m->vram_bank, f); GB_SER(m->wram_bank, f);
    GB_SER(m->boot_rom_enabled, f);
    GB_SER(m->dma_active, f); GB_SER(m->dma_pos, f);
    GB_SER(m->dma_elapsed, f); GB_SER(m->dma_src, f);
    GB_SER(m->hdma_src, f); GB_SER(m->hdma_dst, f);
    GB_SER(m->hdma5, f); GB_SER(m->hdma_hblank, f);
    GB_SER(m->sb, f); GB_SER(m->sc, f);
    GB_SER(m->serial_cycles, f); GB_SER(m->serial_active, f);
    GB_SER(m->serial_len, f);
    GB_SER(m->t_cycles, f);

    timer_serialize(&m->timer, f);
    joypad_serialize(&m->joypad, f);
    ppu_serialize(&m->ppu, f);
    apu_serialize(&m->apu, f);
    cart_serialize(&m->cart, f);
}

void mmu_deserialize(MMU *m, FILE *f)
{
    GB_DESER(m->vram, f);
    GB_DESER(m->wram, f);
    GB_DESER(m->oam, f);
    GB_DESER(m->hram, f);
    GB_DESER(m->io, f);
    GB_DESER(m->ie, f); GB_DESER(m->if_reg, f); GB_DESER(m->open_bus, f);
    GB_DESER(m->cgb, f); GB_DESER(m->double_speed, f);
    GB_DESER(m->vram_bank, f); GB_DESER(m->wram_bank, f);
    GB_DESER(m->boot_rom_enabled, f);
    GB_DESER(m->dma_active, f); GB_DESER(m->dma_pos, f);
    GB_DESER(m->dma_elapsed, f); GB_DESER(m->dma_src, f);
    GB_DESER(m->hdma_src, f); GB_DESER(m->hdma_dst, f);
    GB_DESER(m->hdma5, f); GB_DESER(m->hdma_hblank, f);
    GB_DESER(m->sb, f); GB_DESER(m->sc, f);
    GB_DESER(m->serial_cycles, f); GB_DESER(m->serial_active, f);
    GB_DESER(m->serial_len, f);
    GB_DESER(m->t_cycles, f);

    timer_deserialize(&m->timer, f);
    joypad_deserialize(&m->joypad, f);
    ppu_deserialize(&m->ppu, f);
    apu_deserialize(&m->apu, f);
    cart_deserialize(&m->cart, f);
}
