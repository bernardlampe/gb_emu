/* mmu.h — the memory bus: address decoding and I/O dispatch (L3-L4).
 *
 * Every CPU access in the machine goes through mmu_read8()/mmu_write8(). That is
 * deliberate: it gives one place for open bus, one place for the OAM DMA lockout,
 * one place for the debugger's watchpoints, and one place to add wait states.
 *
 * The devices are embedded by value, so the whole machine is one allocation and a
 * save state is a straight walk of this struct.
 */
#ifndef GB_MMU_H
#define GB_MMU_H

#include "apu.h"
#include "cart.h"
#include "debug.h"
#include "gbdefs.h"
#include "joypad.h"
#include "ppu.h"
#include "timer.h"

/* Address regions, decided by region_of() in mmu.c. */
typedef enum {
    REGION_ROM0,     /* 0000-3FFF: cartridge ROM bank 0, fixed      */
    REGION_ROMX,     /* 4000-7FFF: cartridge ROM bank N, switchable  */
    REGION_VRAM,     /* 8000-9FFF: video RAM, 2 banks on CGB         */
    REGION_SRAM,     /* A000-BFFF: cartridge RAM (or MBC2 nibbles)    */
    REGION_WRAM,     /* C000-DFFF: work RAM, 8 banks on CGB          */
    REGION_ECHO,     /* E000-FDFF: mirror of C000-DDFF                */
    REGION_OAM,      /* FE00-FE9F: sprite attribute table            */
    REGION_UNUSED,   /* FEA0-FEFF: reads as open bus                 */
    REGION_IO,       /* FF00-FF7F: hardware registers                */
    REGION_HRAM,     /* FF80-FFFE: high RAM, always CPU-accessible    */
    REGION_IE        /* FFFF: interrupt enable                        */
} Region;

typedef struct MMU {
    Cart   cart;
    Timer  timer;
    Joypad joypad;
    PPU    ppu;
    APU    apu;

    uint8_t vram[VRAM_BANKS][VRAM_BANK_SIZE];
    uint8_t wram[WRAM_BANKS][WRAM_BANK_SIZE];
    uint8_t oam[OAM_SIZE];
    uint8_t hram[HRAM_SIZE];
    uint8_t io[0x80];        /* backing store for registers with no behaviour */

    uint8_t ie, if_reg;
    uint8_t open_bus;         /* last value read on the bus (FEA0-FEFF etc.) */
    bool    cgb;               /* CGB hardware present */
    bool    double_speed;       /* CGB double-speed mode active */

    /* VRAM / WRAM banking (CGB) */
    unsigned vram_bank;
    unsigned wram_bank;

    /* Boot ROM: while enabled, 0000-00FF reads from boot_rom. */
    const uint8_t *boot_rom;
    size_t         boot_rom_size;
    bool           boot_rom_enabled;

    /* OAM DMA: FF46. While dma_active the CPU may only touch HRAM. */
    bool     dma_active;
    uint32_t dma_pos;      /* bytes copied so far, 0-160 */
    uint32_t dma_elapsed;  /* T-cycles elapsed, one byte per 4 */
    uint16_t dma_src;

    /* HDMA: FF51-FF55 (CGB). */
    uint16_t hdma_src, hdma_dst;
    uint8_t  hdma5;            /* bit 7 = active, bits 0-6 = blocks left - 1 */
    bool     hdma_hblank;

    /* Serial: FF01/FF02. The transfer is emulated with an explicit
     * countdown so the timing is deterministic. */
    uint8_t  sb, sc;
    uint32_t serial_cycles;
    bool     serial_active;
    char     serial_buf[8192];   /* captured test-ROM output (L11) */
    size_t   serial_len;
    FILE    *link_in, *link_out;  /* optional two-instance link (L31) */

    uint64_t t_cycles;          /* total T-cycles, drives the MBC3 clock */

    /* The debugger installs watchpoints through this pointer, set in
     * gb_init(). It is the only reason the bus knows the debugger exists. */
    Debug *debug;
} MMU;

void    mmu_init(MMU *m, bool cgb);
void    mmu_reset(MMU *m);
uint8_t mmu_read8(MMU *m, uint16_t addr);
void    mmu_write8(MMU *m, uint16_t addr, uint8_t v);
uint16_t mmu_read16(MMU *m, uint16_t addr);
void    mmu_write16(MMU *m, uint16_t addr, uint16_t v);

/* Banked VRAM access, used by the PPU for CGB tile attributes. */
uint8_t mmu_read_vram(MMU *m, uint16_t addr, unsigned bank);
void    mmu_write_vram(MMU *m, uint16_t addr, unsigned bank, uint8_t v);

/* Interrupt flags, called by the devices. */
void mmu_if_set(MMU *m, uint8_t bit);
void mmu_if_clear(MMU *m, uint8_t bit);

/* mmu_tick — advance DMA, HDMA and the serial clock by t_cycles. */
void mmu_tick(MMU *m, uint32_t t_cycles);

/* mmu_hdma_hblank — called by the PPU when it enters HBlank (CGB). */
void mmu_hdma_hblank(MMU *m);

/* mmu_set_boot_rom — install an optional boot ROM image. */
void mmu_set_boot_rom(MMU *m, const uint8_t *rom, size_t size);
void mmu_set_link(MMU *m, FILE *in, FILE *out);

void mmu_serialize(MMU *m, FILE *f);
void mmu_deserialize(MMU *m, FILE *f);

#endif /* GB_MMU_H */
