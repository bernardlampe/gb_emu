/* cart.h — cartridge, memory bank controllers, and battery-backed RAM.
 *
 * Lesson mapping: L12 (header parsing), L13 (MBC1/2/5), L14 (MBC3 + RTC + battery).
 *
 * The cartridge is not a file we memcpy into the address space: it is a device with
 * registers, because a write to 0x2000 changes what the CPU sees at 0x4000. Every
 * access therefore goes through cart_read_rom()/cart_read_ram(), never through a
 * raw pointer into the ROM array.
 */
#ifndef GB_CART_H
#define GB_CART_H

#include "gbdefs.h"

#define CART_ROM_MAX (8u * 1024u * 1024u)  /* largest supported ROM */
#define CART_RAM_MAX (128u * 1024u)         /* largest supported cart RAM */
#define CART_PATH_MAX 512

typedef enum {
    MBC_NONE = 0,       /* 32 KiB ROM only */
    MBC1,
    MBC2,               /* 512 * 4 bits of built-in RAM, no external RAM */
    MBC3,               /* + optional real-time clock */
    MBC5,
    MBC_UNSUPPORTED
} MbcType;

typedef struct {
    char    title[17];            /* NUL-terminated, 11 or 16 bytes in the ROM */
    uint8_t cgb_flag;            /* 0x80 = CGB-enhanced, 0xC0 = CGB-only */
    uint8_t cart_type;           /* 0x0147 */
    uint8_t rom_size_code;       /* 0x0148 */
    uint8_t ram_size_code;       /* 0x0149 */
    uint8_t header_checksum;     /* 0x014D, stored */
    uint8_t header_checksum_calc; /* recomputed; must equal the stored one */
    bool    checksum_ok;
    bool    sgb;
    bool    cgb;
    bool    cgb_only;
} CartHeader;

/* MBC3 real-time clock registers. The emulator advances them from emulated
 * cycles, not from wall-clock time, so a replay is bit-identical (see L29). */
typedef struct {
    uint8_t  s, m, h, dl, dh;    /* registers 08-0C as the CPU sees them */
    bool     halt;
    bool     carry;              /* day counter overflow, bit 7 of DH */
    uint64_t latched_s, latched_m, latched_h, latched_dl, latched_dh;
} Rtc;

typedef struct Cart {
    CartHeader header;
    MbcType    mbc;

    uint8_t *rom;                /* owned copy of the ROM image */
    size_t   rom_size;           /* bytes actually present in the file */
    unsigned rom_banks;

    uint8_t *ram;                /* cart RAM (or MBC2's internal 512 nibbles) */
    size_t   ram_size;
    unsigned ram_banks;

    bool has_battery;
    bool has_rtc;

    /* Mapper registers. Keeping these as explicit fields rather than patching
     * pointers into rom[] is what makes save states and the debugger easy. */
    bool     ram_enabled;
    unsigned rom_bank;           /* 0-511, bank visible at 0x4000 */
    unsigned ram_bank;            /* 0-15, or 0-7 for MBC3 RTC select */
    unsigned mode;               /* MBC1 banking mode */
    bool     mbc1_bank_hi;        /* MBC1 bits 5-6 of the bank register */
    bool     mbc1_ram_mode;       /* true once mode 1 has been selected */

    /* MBC3 RTC */
    Rtc      rtc;
    uint8_t  rtc_select;         /* 0 = none, 8-12 = register selected */
    uint8_t  rtc_latch_state;    /* 0 -> 1 on 6000-7FFF latches */
    uint64_t rtc_cycles;         /* emulated T-cycles since reset */
    uint64_t rtc_base_cycles;    /* rtc_cycles value when the RTC was zeroed */

    char     save_path[CART_PATH_MAX];  /* "<rom>.sav" */
    char     rtc_path[CART_PATH_MAX];  /* "<rom>.rtc" */
    bool     dirty;                    /* RAM changed since last save */
} Cart;

/* cart_load — parse the header, pick an MBC, copy the ROM and allocate RAM.
 * Returns false (with a gb_log diagnostic) for a corrupt or unsupported cart. */
bool cart_load(Cart *cart, const uint8_t *rom, size_t rom_size, const char *rom_path);
void cart_free(Cart *cart);

/* Bus interface. Addresses are the CPU-visible ones, already decoded. */
uint8_t cart_read_rom(Cart *cart, uint16_t addr);
void    cart_write_rom(Cart *cart, uint16_t addr, uint8_t v);
uint8_t cart_read_ram(Cart *cart, uint16_t addr);
void    cart_write_ram(Cart *cart, uint16_t addr, uint8_t v);

/* cart_tick — advance the MBC3 clock from emulated cycles. */
void cart_tick(Cart *cart, uint32_t t_cycles);

/* Battery persistence. save_path must have been set by cart_load(). */
void cart_battery_save(Cart *cart);
bool cart_battery_load(Cart *cart);

void cart_serialize(Cart *cart, FILE *f);
void cart_deserialize(Cart *cart, FILE *f);

#endif /* GB_CART_H */
