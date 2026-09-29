/* cart.c — cartridge header, MBC1/2/3/5 banking, RTC and battery RAM (L12-L14).
 *
 * The cartridge is the one device on the bus that is not memory: writing to
 * 0x2000-0x3FFF changes which 16 KiB of ROM appears at 0x4000-0x7FFF, and
 * writing to 0x0000-0x1FFF turns the RAM at 0xA000-0xBFFF on and off. That is
 * why the CPU reaches it only through cart_read_rom()/cart_read_ram() and never
 * through a raw pointer: the mapper registers, not the data, decide the answer.
 *
 * The RTC is driven from emulated T-cycles (cart_tick), never from the host
 * clock, so a replay with the same inputs sees the same clock (see L29).
 *
 * Reference implementation for CS 4XX "System Emulation".
 */
#include "cart.h"

#include <stdlib.h>
#include <string.h>

/* The MBC3 write registers are decoded in section 3 but the RTC itself is
 * explained in section 5, so the two helpers are declared here. */
static void rtc_latch(Cart *cart);
static void rtc_write_register(Cart *cart, uint8_t reg, uint8_t v);

/* ======================================================================
 * 1. Cartridge type table
 *
 * 0x0147 names the hardware, not the game. Everything the mapper does is
 * decided here once, so the hot paths never switch on the raw type byte.
 * ====================================================================== */

/* mbc_from_type — the memory bank controller a type byte implies. */
static MbcType mbc_from_type(uint8_t type)
{
    switch (type) {
    case 0x00: return MBC_NONE;          /* ROM only, no mapper          */
    case 0x01: case 0x02: case 0x03: return MBC1;
    case 0x05: case 0x06: return MBC2;   /* MBC2 always has built-in RAM */
    case 0x0F: case 0x10: case 0x11:
    case 0x12: case 0x13: return MBC3;   /* 0F/10 carry the RTC         */
    case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: return MBC5;
    default:   return MBC_UNSUPPORTED;
    }
}

/* type_has_battery — does this type keep its RAM across a power cycle? */
static bool type_has_battery(uint8_t type)
{
    switch (type) {
    case 0x03: case 0x06: case 0x09: case 0x0D:
    case 0x0F: case 0x10: case 0x13: case 0x1B: case 0x1E:
        return true;
    default:
        return false;
    }
}

/* type_has_rtc — MBC3+TIMER(+RAM+BATTERY) are the clock cartridges. */
static bool type_has_rtc(uint8_t type)
{
    return type == 0x0F || type == 0x10;
}

/* ram_size_from_code — 0x0149 names the external RAM size in bytes.
 * Code 00 means "none" on MBC1/MBC5; MBC2 is handled separately because
 * its 512 nibbles are inside the mapper, not on the board. */
static size_t ram_size_from_code(uint8_t code)
{
    switch (code) {
    case 0x02: return 8u * 1024u;
    case 0x03: return 32u * 1024u;
    case 0x04: return 128u * 1024u;
    case 0x05: return 64u * 1024u;
    default:   return 0u;
    }
}

/* declared_rom_banks — how many 16 KiB banks 0x0148 names.
 * Codes 00-08 are the official ones (2..512 banks); 52/53/54 are the old
 * unofficial codes a handful of carts used. 0 means "code not understood". */
static unsigned declared_rom_banks(uint8_t code)
{
    if (code <= 8u) return 2u << code;
    switch (code) {
    case 0x52u: return 72u;
    case 0x53u: return 80u;
    case 0x54u: return 96u;
    default:    return 0u;
    }
}

/* build_aux_path — replace the ROM's extension with another one.
 * "dir/pokemon.gb" + ".sav" -> "dir/pokemon.sav". A dot inside a directory
 * name is not an extension, so only a dot after the last '/' counts. */
static void build_aux_path(char *dst, size_t dst_size, const char *rom_path,
                          const char *ext)
{
    if (dst == NULL || dst_size == 0u) return;
    dst[0] = '\0';
    if (rom_path == NULL || rom_path[0] == '\0') return;

    const char *dot = strrchr(rom_path, '.');
    const char *slash = strrchr(rom_path, '/');
    size_t base = strlen(rom_path);
    if (dot != NULL && (slash == NULL || dot > slash)) {
        base = (size_t)(dot - rom_path);
    }
    /* Never let the base fill the buffer: the extension and its terminator
     * still have to fit. */
    if (base > dst_size - 1u) base = dst_size - 1u;

    (void)snprintf(dst, dst_size, "%.*s%s", (int)base, rom_path, ext);
}

/* ======================================================================
 * 2. Lifecycle
 * ====================================================================== */

bool cart_load(Cart *cart, const uint8_t *rom, size_t rom_size,
               const char *rom_path)
{
    if (cart == NULL || rom == NULL) {
        gb_log("cartridge: no image given");
        return false;
    }

    /* The header occupies 0x0100-0x014F; a shorter file cannot be a cartridge
     * and indexing it would read past the buffer. */
    if (rom_size < 0x150u) {
        gb_log("cartridge is only %zu bytes: too small to hold a header",
               rom_size);
        return false;
    }

    /* Loading a second cartridge over this one must not leak the first: the
     * buffers are owned by this struct and freed here first. */
    free(cart->rom);
    free(cart->ram);
    cart->rom = NULL;
    cart->ram = NULL;
    cart->rom_size = 0;
    cart->ram_size = 0;
    cart->rom_banks = 0;
    cart->ram_banks = 0;

    /* The cartridge owns its ROM copy. Holding the caller's pointer would dangle
     * as soon as the host closes the file it read the image from. */
    cart->rom = malloc(rom_size);
    if (cart->rom == NULL) {
        gb_log("cartridge: cannot allocate %zu bytes for the ROM", rom_size);
        return false;
    }
    memcpy(cart->rom, rom, rom_size);
    cart->rom_size = rom_size;

    /* --- Header fields ------------------------------------------------- */
    CartHeader *h = &cart->header;
    memset(h, 0, sizeof(*h));

    /* The title is 16 bytes at 0x0134; the extra byte in title[17] is forced
     * to NUL so a title without one cannot overrun gb_log("%s"). */
    memcpy(h->title, cart->rom + 0x134, 16);
    h->title[16] = '\0';

    h->cgb_flag = cart->rom[0x143];
    h->cgb      = (h->cgb_flag == 0x80u || h->cgb_flag == 0xC0u);
    h->cgb_only = (h->cgb_flag == 0xC0u);
    h->sgb      = (cart->rom[0x146] == 0x03u);

    h->cart_type     = cart->rom[0x147];
    h->rom_size_code = cart->rom[0x148];
    h->ram_size_code = cart->rom[0x149];
    h->header_checksum = cart->rom[0x14D];

    /* --- Header checksum, 0x0134-0x014C ---------------------------------
     * Pan Docs: x = x - byte - 1 for each byte, then compare with 0x014D.
     * A mismatch means the header was edited or the image is damaged, which is
     * exactly the case worth refusing with a diagnostic. */
    uint8_t x = 0;
    for (size_t i = 0x134u; i <= 0x14Cu; i++) {
        x = (uint8_t)(x - cart->rom[i] - 1u);
    }
    h->header_checksum_calc = x;
    h->checksum_ok = (x == h->header_checksum);
    if (!h->checksum_ok) {
        gb_log("cartridge header checksum mismatch (stored %02X, computed %02X)",
               h->header_checksum, x);
        return false;
    }

    /* --- Mapper -------------------------------------------------------- */
    cart->mbc = mbc_from_type(h->cart_type);
    if (cart->mbc == MBC_UNSUPPORTED) {
        gb_log("unsupported cartridge type %02X ('%s')", h->cart_type, h->title);
        return false;
    }

    /* --- ROM size ------------------------------------------------------ */
    unsigned declared = declared_rom_banks(h->rom_size_code);
    if (declared == 0u) {
        /* Unknown size code: trust the file rather than refusing to run it. */
        declared = (unsigned)(rom_size / 0x4000u);
        gb_log("cartridge: unknown ROM size code %02X, assuming %zu KiB",
               h->rom_size_code, rom_size / 1024u);
    }
    if (rom_size < (size_t)declared * 0x4000u) {
        gb_log("cartridge is %zu bytes but the header declares %zu bytes",
               rom_size, (size_t)declared * 0x4000u);
        return false;
    }
    cart->rom_banks = declared;
    /* A dump shorter than its own header claims still has to run: bank reads are
     * wrapped in cart_read_rom(), so clamping the count here is safe. */
    if (cart->rom_banks > rom_size / 0x4000u) {
        cart->rom_banks = (unsigned)(rom_size / 0x4000u);
    }
    if (cart->rom_banks == 0u) cart->rom_banks = 1u;

    /* --- Cartridge RAM -------------------------------------------------- */
    cart->has_battery = type_has_battery(h->cart_type);
    cart->has_rtc = type_has_rtc(h->cart_type);

    if (cart->mbc == MBC2) {
        /* MBC2's 512 x 4 bits live inside the mapper, so it needs no code
         * from 0x0149 and is always present. */
        cart->ram_size = 512u;
    } else {
        cart->ram_size = ram_size_from_code(h->ram_size_code);
    }
    cart->ram_banks = cart->ram_size / 0x2000u;   /* one bank is 8 KiB */
    if (cart->ram_size > 0u && cart->ram_banks == 0u) cart->ram_banks = 1u;

    if (cart->ram_size > 0u) {
        /* calloc, not malloc: a game that reads its RAM before writing it must
         * not see whatever the host's allocator happened to leave there. */
        cart->ram = calloc(cart->ram_size, 1);
        if (cart->ram == NULL) {
            gb_log("cartridge: cannot allocate %zu bytes for the RAM",
                   cart->ram_size);
            free(cart->rom);
            cart->rom = NULL;
            cart->rom_size = 0;
            return false;
        }
    }

    /* --- Mapper registers and RTC -------------------------------------- */
    cart->ram_enabled = false;   /* cart RAM is off until 0x0A is written  */
    cart->rom_bank = 1;         /* bank 1, not 0, is mapped at 0x4000   */
    cart->ram_bank = 0;
    cart->mode = 0;
    cart->mbc1_bank_hi = false;
    cart->mbc1_ram_mode = false;
    cart->rtc_select = 0;
    cart->rtc_latch_state = 0;
    cart->rtc_cycles = 0;
    cart->rtc_base_cycles = 0;
    memset(&cart->rtc, 0, sizeof(cart->rtc));
    cart->dirty = false;

    /* --- Battery paths -------------------------------------------------- */
    build_aux_path(cart->save_path, sizeof(cart->save_path), rom_path, ".sav");
    build_aux_path(cart->rtc_path, sizeof(cart->rtc_path), rom_path, ".rtc");

    /* Restore a previous save now, so the game sees it from its first
     * instruction. A missing file is the normal first run, not an error. */
    if (cart->has_battery) (void)cart_battery_load(cart);

    return true;
}

void cart_free(Cart *cart)
{
    if (cart == NULL) return;
    free(cart->rom);
    free(cart->ram);
    cart->rom = NULL;
    cart->ram = NULL;
    cart->rom_size = 0;
    cart->ram_size = 0;
    cart->rom_banks = 0;
    cart->ram_banks = 0;
}

/* ======================================================================
 * 3. ROM reads and the mapper write registers
 * ====================================================================== */

uint8_t cart_read_rom(Cart *cart, uint16_t addr)
{
    if (cart == NULL || cart->rom == NULL || cart->rom_size == 0u) return 0xFFu;

    size_t off;
    if (addr < 0x4000u) {
        off = addr;   /* 0x0000-0x3FFF is always bank 0 */
    } else {
        unsigned bank = cart->rom_bank;
        /* MBC1's mode 1 hides the two high bits of the bank number, which is
         * what makes mode 1 the "large RAM, 32 KiB ROM" configuration. */
        if (cart->mbc == MBC1 && cart->mode == 0u && cart->mbc1_bank_hi) {
            bank |= 0x20u;
        }
        off = (size_t)bank * 0x4000u + (size_t)(addr - 0x4000u);
    }

    /* A dump shorter than its header claims exists, so the offset wraps into
     * the image instead of reading past its end. */
    if (off >= cart->rom_size) off %= cart->rom_size;
    return cart->rom[off];
}

/* mbc1_write — RAM enable, bank low 5 bits, bank high bit / RAM bank, mode. */
static void mbc1_write(Cart *cart, uint16_t addr, uint8_t v)
{
    if (addr < 0x2000u) {
        /* 0000-1FFF: only 0x0A in the low nibble enables cart RAM. */
        cart->ram_enabled = ((v & 0x0Fu) == 0x0Au);
    } else if (addr < 0x4000u) {
        /* 2000-3FFF: the low 5 bits of the bank number. Bank 0 is not
         * mappable here, so the classic quirk turns a written 0 into 1. */
        cart->rom_bank = v & 0x1Fu;
        if (cart->rom_bank == 0u) cart->rom_bank = 1u;
    } else if (addr < 0x6000u) {
        /* 4000-5FFF: one register with two jobs. In mode 0 it supplies
         * bit 5 of the ROM bank, in mode 1 the RAM bank. Both fields are
         * updated so the register keeps its value across a mode switch. */
        cart->ram_bank = v & 0x03u;
        cart->mbc1_bank_hi = (v & 0x01u) != 0u;
    } else {
        /* 6000-7FFF: mode select. Mode 0 forces RAM bank 0, so the 8 KiB of
         * cart RAM at 0xA000 are mirrored over 0xC000-0xDFFF style ROM area. */
        cart->mode = v & 0x01u;
        if (cart->mode != 0u) {
            cart->mbc1_ram_mode = true;   /* sticky: mode 1 was used */
        } else {
            cart->ram_bank = 0u;
        }
    }
}

/* mbc2_write — address bit 8 picks which of the two registers is written. */
static void mbc2_write(Cart *cart, uint16_t addr, uint8_t v)
{
    if ((addr & 0x0100u) != 0u) {
        /* Bit 8 set: the ROM bank's low 4 bits. Bank 0 is not mappable. */
        cart->rom_bank = v & 0x0Fu;
        if (cart->rom_bank == 0u) cart->rom_bank = 1u;
    } else {
        /* Bit 8 clear: the RAM enable. */
        cart->ram_enabled = ((v & 0x0Fu) == 0x0Au);
    }
}

/* mbc3_write — RAM/RTC enable, 7-bit ROM bank, RAM/RTC select, latch. */
static void mbc3_write(Cart *cart, uint16_t addr, uint8_t v)
{
    if (addr < 0x2000u) {
        /* 0000-1FFF: 0x0A enables cart RAM and the RTC together. */
        cart->ram_enabled = ((v & 0x0Fu) == 0x0Au);
    } else if (addr < 0x4000u) {
        /* 2000-3FFF: the 7-bit bank number; bank 0 becomes bank 1. */
        cart->rom_bank = v & 0x7Fu;
        if (cart->rom_bank == 0u) cart->rom_bank = 1u;
    } else if (addr < 0x6000u) {
        /* 4000-5FFF: 0x00-0x03 select cart RAM, 0x08-0x0C an RTC
         * register. Any other value leaves the current selection alone. */
        if (v <= 0x03u) {
            cart->ram_bank = v & 0x03u;
            cart->rtc_select = 0;
        } else if (v >= 0x08u && v <= 0x0Cu) {
            cart->rtc_select = v;
        }
    } else {
        /* 6000-7FFF: a write of 0 then 1 copies the running clock into the
         * latched registers, so a game reads one consistent time. */
        if (v == 0x00u) {
            cart->rtc_latch_state = 0;
        } else if (v == 0x01u && cart->rtc_latch_state == 0u) {
            rtc_latch(cart);
            cart->rtc_latch_state = 1;
        }
    }
}

/* mbc5_write — 9-bit ROM bank and 4-bit RAM bank. */
static void mbc5_write(Cart *cart, uint16_t addr, uint8_t v)
{
    if (addr < 0x2000u) {
        cart->ram_enabled = ((v & 0x0Fu) == 0x0Au);
    } else if (addr < 0x3000u) {
        /* 2000-2FFF: the low 8 bits of the bank number. Bank 0 is legal on
         * MBC5, so there is no 0 -> 1 quirk here. */
        cart->rom_bank = (cart->rom_bank & 0x100u) | (unsigned)v;
    } else if (addr < 0x4000u) {
        /* 3000-3FFF: bit 8 of the bank number. */
        cart->rom_bank = (cart->rom_bank & 0xFFu) | ((unsigned)(v & 0x01u) << 8);
    } else if (addr < 0x6000u) {
        /* 4000-5FFF: the RAM bank, 0-15. */
        cart->ram_bank = v & 0x0Fu;
    }
    /* 6000-7FFF is not decoded on MBC5. */
}

void cart_write_rom(Cart *cart, uint16_t addr, uint8_t v)
{
    if (cart == NULL) return;

    switch (cart->mbc) {
    case MBC1: mbc1_write(cart, addr, v); break;
    case MBC2: mbc2_write(cart, addr, v); break;
    case MBC3: mbc3_write(cart, addr, v); break;
    case MBC5: mbc5_write(cart, addr, v); break;
    case MBC_NONE:
    case MBC_UNSUPPORTED:
    default:
        break;   /* ROM only has no registers: the writes go nowhere */
    }
}

/* ======================================================================
 * 4. Cartridge RAM
 * ====================================================================== */

uint8_t cart_read_ram(Cart *cart, uint16_t addr)
{
    if (cart == NULL || !cart->ram_enabled) return 0xFFu;

    /* MBC3: the same window can select an RTC register instead of RAM, and
     * an RTC-only cartridge has no RAM at all, so this is checked first. */
    if (cart->mbc == MBC3 && cart->rtc_select >= 8u && cart->rtc_select <= 12u) {
        switch (cart->rtc_select) {
        case 8:  return (uint8_t)cart->rtc.latched_s;
        case 9:  return (uint8_t)cart->rtc.latched_m;
        case 10: return (uint8_t)cart->rtc.latched_h;
        case 11: return (uint8_t)cart->rtc.latched_dl;
        default: /* 12: the day counter's high bits. Only bit 0 is the ninth
                  * day bit; bits 1-5 are unused and read as 1, bit 6 is the
                  * halt flag and bit 7 the day-counter carry. */
            return (uint8_t)(cart->rtc.latched_dh | 0x3Eu);
        }
    }

    if (cart->ram == NULL || cart->ram_size == 0u) return 0xFFu;

    if (cart->mbc == MBC2) {
        /* MBC2 keeps one nibble per byte and mirrors its 512 nibbles over
         * the whole 0xA000-0xBFFF window. */
        return (uint8_t)(cart->ram[addr & 0x1FFu] & 0x0Fu);
    }

    /* ram_bank is masked by the bank count: a cart with one 8 KiB bank that
     * is asked for bank 3 still reads bank 0 instead of past the buffer. */
    unsigned bank = (cart->ram_banks != 0u) ? (cart->ram_bank % cart->ram_banks) : 0u;
    return cart->ram[(size_t)bank * 0x2000u + (addr & 0x1FFFu)];
}

void cart_write_ram(Cart *cart, uint16_t addr, uint8_t v)
{
    if (cart == NULL || !cart->ram_enabled) return;

    /* MBC3 writes its RTC registers through the same window as RAM. */
    if (cart->mbc == MBC3 && cart->rtc_select >= 8u && cart->rtc_select <= 12u) {
        rtc_write_register(cart, cart->rtc_select, v);
        cart->dirty = true;
        return;
    }

    if (cart->ram == NULL || cart->ram_size == 0u) return;

    if (cart->mbc == MBC2) {
        cart->ram[addr & 0x1FFu] = v & 0x0Fu;   /* 4 bits per byte */
    } else {
        unsigned bank = (cart->ram_banks != 0u) ? (cart->ram_bank % cart->ram_banks) : 0u;
        cart->ram[(size_t)bank * 0x2000u + (addr & 0x1FFFu)] = v;
    }

    /* The RAM differs from the .sav file on disk until the next save. */
    cart->dirty = true;
}

/* ======================================================================
 * 5. MBC3 real-time clock
 *
 * The clock is a pure function of the emulated cycle count, so it survives
 * a save state exactly and a replay produces the same time twice.
 * ====================================================================== */

/* rtc_latch — copy the running registers into the latched ones. */
static void rtc_latch(Cart *cart)
{
    cart->rtc.latched_s = cart->rtc.s;
    cart->rtc.latched_m = cart->rtc.m;
    cart->rtc.latched_h = cart->rtc.h;
    cart->rtc.latched_dl = cart->rtc.dl;
    cart->rtc.latched_dh = cart->rtc.dh;
}

/* rtc_rebase — re-anchor the cycle base after the CPU writes a register.
 * cart_tick() rebuilds the clock from (rtc_cycles - rtc_base_cycles), so moving
 * the base is what makes a written register survive the next tick. */
static void rtc_rebase(Cart *cart)
{
    uint64_t days = (uint64_t)cart->rtc.dl
                  | ((uint64_t)(cart->rtc.dh & 0x01u) << 8);
    uint64_t secs = days * 86400u
                  + (uint64_t)cart->rtc.h * 3600u
                  + (uint64_t)cart->rtc.m * 60u
                  + cart->rtc.s;
    /* The subtraction wraps when the written time is ahead of the emulated
     * clock; unsigned arithmetic is defined, so this is not UB. */
    cart->rtc_base_cycles = cart->rtc_cycles - secs * CPU_CLOCK_HZ;
}

/* rtc_write_register — write one of the RTC registers 08-0C. */
static void rtc_write_register(Cart *cart, uint8_t reg, uint8_t v)
{
    switch (reg) {
    case 8:  cart->rtc.s  = v & 0x3Fu; break;   /* 0-59 seconds */
    case 9:  cart->rtc.m  = v & 0x3Fu; break;   /* 0-59 minutes */
    case 10: cart->rtc.h  = v & 0x1Fu; break;   /* 0-23 hours   */
    case 11: cart->rtc.dl = v; break;             /* day counter, low 8 bits */
    default: /* 12: bit 0 is the ninth day bit, bit 6 halts the clock and
              * bit 7 is the day-counter carry. Bits 1-5 are unused. */
        cart->rtc.halt  = (v & 0x40u) != 0u;
        cart->rtc.carry = (v & 0x80u) != 0u;
        cart->rtc.dh = (uint8_t)(v & 0xC1u);
        break;
    }
    rtc_rebase(cart);
}

void cart_tick(Cart *cart, uint32_t t_cycles)
{
    if (cart == NULL) return;

    /* Count emulated T-cycles: the clock must be identical on a replay, so it
     * must never read the host clock or sleep. */
    cart->rtc_cycles += t_cycles;

    /* A halted clock stands still, exactly as register 0x0C bit 6 promises. */
    if (!cart->has_rtc || cart->rtc.halt) return;

    uint64_t secs = (cart->rtc_cycles - cart->rtc_base_cycles) / CPU_CLOCK_HZ;

    /* Split the elapsed seconds into the day counter and time of day. */
    uint64_t days = secs / 86400u;
    uint32_t sod = (uint32_t)(secs % 86400u);

    cart->rtc.s = (uint8_t)(sod % 60u);
    cart->rtc.m = (uint8_t)((sod / 60u) % 60u);
    cart->rtc.h = (uint8_t)(sod / 3600u);

    /* The day counter is 9 bits. Bit 9 overflows into the carry flag, which
     * stays set until register 0x0C clears it. */
    if (days > 511u) {
        cart->rtc.carry = true;
        days &= 0x1FFu;
    }
    cart->rtc.dl = (uint8_t)(days & 0xFFu);
    cart->rtc.dh = (uint8_t)((days >> 8) & 0x01u);
    if (cart->rtc.carry) cart->rtc.dh |= 0x80u;
}

/* ======================================================================
 * 6. Battery-backed files
 *
 * Two small files next to the ROM: "<rom>.sav" for the RAM and
 * "<rom>.rtc" for the clock. Both are native-endian, same-build only, the
 * same rule the save states follow.
 * ====================================================================== */

/* rtc_save — write the RTC registers and cycle counters. */
static void rtc_save(const Cart *cart)
{
    FILE *f = fopen(cart->rtc_path, "wb");
    if (f == NULL) {
        gb_log("cartridge: cannot write RTC file '%s'", cart->rtc_path);
        return;
    }

    const uint8_t regs[5] = { cart->rtc.s, cart->rtc.m, cart->rtc.h,
                              cart->rtc.dl, cart->rtc.dh };
    uint8_t flags = (uint8_t)((cart->rtc.halt ? 0x01u : 0u)
                           | (cart->rtc.carry ? 0x02u : 0u));

    /* Written field by field, never as a struct: a struct dump would also
     * write the padding bytes between its members. */
    (void)fwrite(regs, 1, sizeof(regs), f);
    (void)fwrite(&flags, 1, 1, f);
    (void)fwrite(&cart->rtc_cycles, sizeof(cart->rtc_cycles), 1, f);
    (void)fwrite(&cart->rtc_base_cycles, sizeof(cart->rtc_base_cycles), 1, f);
    fclose(f);
}

/* rtc_load — read the RTC file back; false when it is absent or short. */
static bool rtc_load(Cart *cart)
{
    FILE *f = fopen(cart->rtc_path, "rb");
    if (f == NULL) return false;   /* no clock file yet: normal first run */

    uint8_t regs[5] = { 0, 0, 0, 0, 0 };
    uint8_t flags = 0;
    bool ok = fread(regs, 1, sizeof(regs), f) == sizeof(regs)
           && fread(&flags, 1, 1, f) == 1
           && fread(&cart->rtc_cycles, sizeof(cart->rtc_cycles), 1, f) == 1
           && fread(&cart->rtc_base_cycles, sizeof(cart->rtc_base_cycles), 1, f) == 1;
    fclose(f);

    if (!ok) {
        gb_log("cartridge: RTC file '%s' is truncated, ignoring it",
               cart->rtc_path);
        return false;
    }

    cart->rtc.s  = regs[0];
    cart->rtc.m  = regs[1];
    cart->rtc.h  = regs[2];
    cart->rtc.dl = regs[3];
    cart->rtc.dh = regs[4];
    cart->rtc.halt  = (flags & 0x01u) != 0u;
    cart->rtc.carry = (flags & 0x02u) != 0u;

    /* A game that reads before its first latch must still see a clock. */
    rtc_latch(cart);
    return true;
}

void cart_battery_save(Cart *cart)
{
    if (cart == NULL) return;

    if (cart->has_battery && cart->ram != NULL && cart->ram_size > 0u
        && cart->dirty) {
        FILE *f = fopen(cart->save_path, "wb");
        if (f == NULL) {
            gb_log("cartridge: cannot write save file '%s'", cart->save_path);
        } else {
            /* The .sav file is exactly the RAM: no header, so it is the file
             * a real cartridge would have seen. */
            size_t n = fwrite(cart->ram, 1, cart->ram_size, f);
            if (n != cart->ram_size) {
                gb_log("cartridge: short write to '%s'", cart->save_path);
            } else {
                cart->dirty = false;
            }
            fclose(f);
        }
    }

    /* The clock is saved even when the RAM has not changed. */
    if (cart->has_rtc) rtc_save(cart);
}

bool cart_battery_load(Cart *cart)
{
    if (cart == NULL) return false;

    bool loaded = false;

    if (cart->has_battery && cart->ram != NULL && cart->ram_size > 0u) {
        FILE *f = fopen(cart->save_path, "rb");
        if (f != NULL) {
            /* Measure before copying: a save from a different game (or a
             * half-written file) must not be pasted over this cart's RAM. */
            long size = -1;
            if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
            if (size >= 0 && (size_t)size == cart->ram_size) {
                rewind(f);
                if (fread(cart->ram, 1, cart->ram_size, f) == cart->ram_size) {
                    loaded = true;
                    cart->dirty = false;   /* disk and RAM agree again */
                }
            } else if (size >= 0) {
                gb_log("cartridge: save file '%s' is %ld bytes, expected %zu; ignoring it",
                       cart->save_path, size, cart->ram_size);
            }
            fclose(f);
        }
    }

    /* The RTC is independent of the RAM: a game can have either. */
    if (cart->has_rtc) (void)rtc_load(cart);

    return loaded;
}

/* ======================================================================
 * 7. Save states
 *
 * Only what cannot be reconstructed is stored: the mapper registers, the RTC
 * and the RAM contents. The ROM image comes back from the file, so writing it
 * into every save state would waste megabytes for nothing.
 * ====================================================================== */

void cart_serialize(Cart *cart, FILE *f)
{
    if (cart == NULL || f == NULL) return;

    GB_SER(cart->ram_enabled, f);
    GB_SER(cart->rom_bank, f);
    GB_SER(cart->ram_bank, f);
    GB_SER(cart->mode, f);
    GB_SER(cart->mbc1_bank_hi, f);
    GB_SER(cart->mbc1_ram_mode, f);

    GB_SER(cart->rtc_select, f);
    GB_SER(cart->rtc_latch_state, f);

    GB_SER(cart->rtc.s, f);
    GB_SER(cart->rtc.m, f);
    GB_SER(cart->rtc.h, f);
    GB_SER(cart->rtc.dl, f);
    GB_SER(cart->rtc.dh, f);
    GB_SER(cart->rtc.halt, f);
    GB_SER(cart->rtc.carry, f);
    GB_SER(cart->rtc.latched_s, f);
    GB_SER(cart->rtc.latched_m, f);
    GB_SER(cart->rtc.latched_h, f);
    GB_SER(cart->rtc.latched_dl, f);
    GB_SER(cart->rtc.latched_dh, f);
    GB_SER(cart->rtc_cycles, f);
    GB_SER(cart->rtc_base_cycles, f);

    GB_SER(cart->dirty, f);

    /* The RAM blob is prefixed with its length, so loading a state saved from
     * a different cartridge is detected instead of corrupting the buffer. */
    GB_SER(cart->ram_size, f);
    if (cart->ram != NULL && cart->ram_size > 0u) {
        (void)fwrite(cart->ram, 1, cart->ram_size, f);
    }
}

void cart_deserialize(Cart *cart, FILE *f)
{
    if (cart == NULL || f == NULL) return;

    GB_DESER(cart->ram_enabled, f);
    GB_DESER(cart->rom_bank, f);
    GB_DESER(cart->ram_bank, f);
    GB_DESER(cart->mode, f);
    GB_DESER(cart->mbc1_bank_hi, f);
    GB_DESER(cart->mbc1_ram_mode, f);

    GB_DESER(cart->rtc_select, f);
    GB_DESER(cart->rtc_latch_state, f);

    GB_DESER(cart->rtc.s, f);
    GB_DESER(cart->rtc.m, f);
    GB_DESER(cart->rtc.h, f);
    GB_DESER(cart->rtc.dl, f);
    GB_DESER(cart->rtc.dh, f);
    GB_DESER(cart->rtc.halt, f);
    GB_DESER(cart->rtc.carry, f);
    GB_DESER(cart->rtc.latched_s, f);
    GB_DESER(cart->rtc.latched_m, f);
    GB_DESER(cart->rtc.latched_h, f);
    GB_DESER(cart->rtc.latched_dl, f);
    GB_DESER(cart->rtc.latched_dh, f);
    GB_DESER(cart->rtc_cycles, f);
    GB_DESER(cart->rtc_base_cycles, f);

    GB_DESER(cart->dirty, f);

    size_t ram_size = 0;
    GB_DESER(ram_size, f);
    if (cart->ram != NULL && ram_size == cart->ram_size) {
        (void)fread(cart->ram, 1, cart->ram_size, f);
    } else if (ram_size > 0u) {
        /* The blob belongs to another cartridge: skip it so the stream stays
         * aligned for whatever is serialized after the cartridge. */
        gb_log("save state holds %zu bytes of cart RAM, this cart has %zu",
               ram_size, cart->ram_size);
        (void)fseek(f, (long)ram_size, SEEK_CUR);
    }
}
