/* apu.c — the four-channel sound unit (L24-L27).
 *
 * The APU is four independent DACs feeding a stereo mixer. It is clocked at
 * CPU/4 = 1.048576 MHz (APU_CLOCK_HZ): every channel counts down a T-cycle
 * timer and, when that timer expires, steps its waveform. The host drains the
 * stereo ring buffer and resamples; this file never includes SDL and never reads
 * a wall clock, so the same core runs headless in CI and in the browser.
 *
 * The 512 Hz frame sequencer (length / sweep / envelope) is NOT counted here.
 * On hardware it is the falling edge of DIV bit 4, so the timer module calls
 * apu_frame_step() and the APU and DIV share one counter. Modeling it with a
 * second divider is the classic source of "sound drifts by a few percent" bugs.
 *
 * This file is teaching material: every non-obvious line is commented.
 */
#include "apu.h"
#include "mmu.h"

#include <string.h>

/* --- Duty waveform ------------------------------------------------------
 * Each duty setting is one period of an 8-step square wave; the channel outputs
 * the pattern bit at its current position. 12.5 % / 25 % / 50 % / 75 %.
 */
static const uint8_t duty_table[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },   /* 12.5 % */
    { 1, 0, 0, 0, 0, 0, 0, 1 },   /* 25 %   */
    { 1, 0, 0, 0, 0, 1, 1, 1 },   /* 50 %   */
    { 0, 1, 1, 1, 1, 1, 1, 0 }    /* 75 %   */
};

/* --- Post-boot register values ------------------------------------------
 * The boot ROM leaves NR10..NR52 in this state (Pan Docs, "Audio Registers").
 * A ROM that skips the boot ROM must see the same machine, so apu_reset()
 * copies this table into a->nr. Index = register address - 0x10.
 */
static const uint8_t nr_post_boot[0x20] = {
    /* NR10  NR11  NR12  NR13  NR14  ----  NR21  NR22 */
    0x80, 0xBF, 0xF3, 0xFF, 0xBF, 0xFF, 0x3F, 0x00,
    /* NR23  NR24  NR30  NR31  NR32  NR33  NR34  ---- */
    0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF,
    /* NR41  NR42  NR43  NR44  NR50  NR51  NR52  ---- */
    0xFF, 0x00, 0x00, 0xBF, 0x77, 0xF3, 0xF1, 0xFF,
    /* FF27-FF2F are unused and read 0xFF */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

/* Unused bits of NR10..NR51 read back as a fixed value on the DMG, and the
 * unused FF15-FF1F / FF27-FF2F ranges read 0xFF. Reads therefore combine
 * the stored byte with these two masks; dmg_sound 01-registers checks exactly
 * this behaviour. */
static const uint8_t nr_fixed[0x20] = {
    0x80, 0x00, 0x00, 0x00, 0x38, 0xFF, 0x00, 0x00,   /* NR10 NR11 NR12 NR13 NR14 FF15 NR21 NR22 */
    0x00, 0x38, 0x7F, 0x00, 0x9F, 0x00, 0x38, 0xFF,   /* NR23 NR24 NR30 NR31 NR32 NR33 NR34 FF1F */
    0xC0, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0xFF,   /* NR41 NR42 NR43 NR44 NR50 NR51 NR52 FF27 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* FF28..FF2F */
};

static const uint8_t nr_writable[0x20] = {
    0x7F, 0xFF, 0xFF, 0xFF, 0xC7, 0x00, 0xFF, 0xFF,   /* NR10 NR11 NR12 NR13 NR14 FF15 NR21 NR22 */
    0xFF, 0xC7, 0x80, 0xFF, 0x60, 0xFF, 0xC7, 0x00,   /* NR23 NR24 NR30 NR31 NR32 NR33 NR34 FF1F */
    0x3F, 0xFF, 0xFF, 0xC0, 0x77, 0xFF, 0x80, 0x00,   /* NR41 NR42 NR43 NR44 NR50 NR51 NR52 FF27 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* FF28..FF2F */
};

/* --- Small helpers ------------------------------------------------------ */

/* noise_timer_reload — T-cycles between LFSR shifts.
 *
 * f = 524288/r Hz with r = divisor * 2^(shift+1), and the divisor code is the
 * divisor except that code 0 means 0.5 (it is neither silent nor a divide by
 * zero). One shift therefore lasts 4194304/f = 8*r T-cycles, i.e.
 * (divisor << shift) * 16, and 8 << shift for the 0.5 case.
 *
 * Using code * 16 as the divisor makes every noise period sixteen times too
 * long, i.e. the noise four octaves flat: a hi-hat becomes a rumble. */
static unsigned noise_timer_reload(const Noise *n)
{
    if (n->divisor_code == 0u)
        return 8u << n->clock_shift;                /* divisor 0.5 */
    return (n->divisor_code << n->clock_shift) * 16u;
}

/* square_timer_reload — T-cycles per duty step.
 *
 * Derivation from the hardware frequency, which Pan Docs gives as
 * f = 131072 / (2048 - period) Hz. One duty step is an eighth of a waveform
 * period, so it lasts 1 / (8f) = (2048 - period) / 1048576 seconds, which at
 * 4194304 T-cycles per second is (2048 - period) * 4 T-cycles.
 *
 * apu_tick() is given CPU T-cycles, so this is already the right unit and the
 * factor really is four: a channel whose timer counted APU cycles instead would
 * play two octaves flat. */
static unsigned square_timer_reload(const Square *s)
{
    if (s->period >= 2048u) return 0u;   /* reset value 2048: silent channel */
    return (2048u - s->period) * 4u;
}

/* wave_timer_reload — the wave channel's frequency is half the square's, i.e.
 * f = 65536 / (2048 - period) Hz, and it has 32 sample steps per period
 * instead of 8, so one step lasts (2048 - period) * 2 T-cycles. */
static unsigned wave_timer_reload(const Wave *w)
{
    if (w->period >= 2048u) return 0u;
    return (2048u - w->period) * 2u;
}

/* length_clock — the length counter ticks at 256 Hz. It only runs when the
 * length-enable bit is set, and when it reaches 0 the channel is switched off. */
static void length_clock(bool length_enable, unsigned *length, bool *enabled)
{
    if (!length_enable || *length == 0u) return;
    (*length)--;
    if (*length == 0u) *enabled = false;
}

/* env_clock — the envelope ticks at 64 Hz. Period 0 disables it entirely. */
static void env_clock(uint8_t *volume, bool dir, unsigned period, unsigned *timer)
{
    if (period == 0u) return;             /* NRx2 period 0 = envelope off */
    if (*timer > 0u) (*timer)--;
    if (*timer != 0u) return;
    *timer = period;
    if (dir) {
        if (*volume < 15u) (*volume)++;
    } else {
        if (*volume > 0u) (*volume)--;
    }
}

/* --- Channel timers (one T-cycle each) ---------------------------------- */

/* square_tick — count down to the next duty step, then advance the 8-step
 * position. The step is what makes the duty waveform move, so the timer reload
 * must use the full period. */
static void square_tick(Square *s)
{
    if (s->timer > 0u) s->timer--;
    if (s->timer != 0u) return;
    s->timer = square_timer_reload(s);
    s->duty_pos = (uint8_t)((s->duty_pos + 1u) & 7u);
}

/* wave_tick — same idea, but 32 samples and half the period units. */
static void wave_tick(Wave *w)
{
    if (w->timer > 0u) w->timer--;
    if (w->timer != 0u) return;
    w->timer = wave_timer_reload(w);
    w->pos = (w->pos + 1u) & 31u;
}

/* noise_tick — count down, then clock the LFSR. The feedback bit is
 * bit0 XOR bit1; it is shifted in at bit 14 (bit 6 as well in 7-bit mode),
 * which is the standard 15-bit maximal-length sequence. */
static void noise_tick(Noise *n)
{
    if (n->timer > 0u) n->timer--;
    if (n->timer != 0u) return;
    n->timer = noise_timer_reload(n);

    uint16_t feedback = (uint16_t)((n->lfsr ^ (n->lfsr >> 1u)) & 1u);
    n->lfsr = (uint16_t)((n->lfsr >> 1u) | (feedback << 14u));
    if (n->width7) {
        /* 7-bit mode taps bit 6 instead of bit 14: the sequence is shorter. */
        n->lfsr = (uint16_t)((n->lfsr & ~(1u << 6u)) | (feedback << 6u));
    }
}

/* --- Triggers ----------------------------------------------------------- */

/* square_trigger — a write of 1 to NRx4 bit 7 restarts the channel. */
static void square_trigger(APU *a, Square *s, unsigned nr11_idx)
{
    /* Reload the period timer from the current period register. */
    s->timer = square_timer_reload(s);
    /* The length counter reloads from its load value only if it has run out. */
    if (s->length == 0u) s->length = 64u - (unsigned)(a->nr[nr11_idx] & 0x3Fu);
    /* A channel only runs while its DAC is on; otherwise it stays silent. */
    s->enabled = s->dac_enabled;
    /* Restart the envelope at the initial volume. */
    s->volume = s->env_initial;
    s->env_timer = s->env_period;
}

/* square_sweep_trigger — arm the sweep and run the immediate overflow
 * check. The check must use the *new* period, which is why it happens here. */
static void square_sweep_trigger(Square *s)
{
    s->sweep_timer = s->sweep_period ? s->sweep_period : 8u;
    s->sweep_enabled = (s->sweep_period | s->sweep_shift) != 0u;
    s->sweep_neg_used = false;
    if (s->sweep_shift != 0u) {
        unsigned delta = s->period >> s->sweep_shift;
        int np = s->sweep_negate ? (int)s->period - (int)delta
                                 : (int)s->period + (int)delta;
        if (np > 2047) s->enabled = false;   /* overflow: channel off */
    }
}

/* wave_trigger — reload the timer and length, but NOT the sample position:
 * on the DMG the wave channel keeps playing from where it was, which is what
 * dmg_sound tests 09 and 10 check. */
static void wave_trigger(APU *a)
{
    a->wave.timer = wave_timer_reload(&a->wave);
    if (a->wave.length == 0u) a->wave.length = 256u - (unsigned)a->nr[0x0Bu];
    a->wave.enabled = a->wave.dac_enabled;
}

/* noise_trigger — reload the timer, reset the LFSR to all ones and restart
 * the envelope. */
static void noise_trigger(APU *a)
{
    a->noise.timer = noise_timer_reload(&a->noise);
    a->noise.lfsr = 0x7FFFu;
    if (a->noise.length == 0u) a->noise.length = 64u - (unsigned)(a->nr[0x10u] & 0x3Fu);
    a->noise.enabled = a->noise.dac_enabled;
    a->noise.volume = a->noise.env_initial;
    a->noise.env_timer = a->noise.env_period;
}

/* --- Frame sequencer ---------------------------------------------------- */

/* square_clock_sweep — the sweep unit ticks at 128 Hz. When the timer
 * expires the period is nudged up or down, and if it leaves the 11-bit range
 * the channel is switched off. */
static void square_clock_sweep(Square *s)
{
    if (s->sweep_timer > 0u) s->sweep_timer--;
    if (s->sweep_timer != 0u) return;
    s->sweep_timer = s->sweep_period ? s->sweep_period : 8u;
    if (!s->sweep_enabled || s->sweep_period == 0u) return;

    unsigned delta = s->period >> s->sweep_shift;
    /* Signed arithmetic so a negating sweep can go below zero without wrapping. */
    int np = s->sweep_negate ? (int)s->period - (int)delta
                             : (int)s->period + (int)delta;
    if (np < 0 || np > 2047) {
        s->enabled = false;              /* overflow: the channel stops */
        return;
    }
    if (s->sweep_shift != 0u) s->period = (unsigned)np;
    if (s->sweep_negate) s->sweep_neg_used = true;
}

/* --- Register read/write ------------------------------------------------ */

/* nr_read — the stored byte with its unused bits replaced by their DMG
 * read-back value. NR52 is computed separately in apu_read_reg(). */
static uint8_t nr_read(const APU *a, unsigned idx)
{
    return (uint8_t)(nr_fixed[idx] | (a->nr[idx] & nr_writable[idx]));
}

void apu_init(APU *a)
{
    memset(a, 0, sizeof(*a));
    a->sample_rate = SAMPLE_RATE;
    a->hp_cap_l = 0.0f;
    a->hp_cap_r = 0.0f;
}

void apu_reset(APU *a)
{
    /* Power the unit down: exactly what writing 0 to NR52 bit 7 does. */
    a->nr52 = 0u;
    a->power = false;
    a->sq1.enabled = false;
    a->sq2.enabled = false;
    a->wave.enabled = false;
    a->noise.enabled = false;

    /* The host owns the ring buffer; a reset just drops what is in it. */
    a->head = 0u;
    a->tail = 0u;

    a->sample_acc = 0u;
    a->fs_div = 0u;
    a->fs_step = 0u;

    /* Post-boot state, straight from Pan Docs: the boot ROM leaves the
     * registers here, so a ROM that skips it sees the same machine. */
    memcpy(a->nr, nr_post_boot, sizeof(a->nr));
    a->nr50 = a->nr[0x14u];
    a->nr51 = a->nr[0x15u];
    a->nr52 = a->nr[0x16u];
    a->power = true;

    /* Square 1 is the only channel left running, at full volume. */
    a->sq1.duty = 2u;
    a->sq1.volume = 15u;
    a->sq1.env_initial = 15u;
    a->sq1.period = 2048u;
    a->sq1.timer = 4u;
    a->sq1.enabled = true;

    /* Square 2, wave and noise start silent. */
    a->sq2.volume = 0u;
    a->sq2.enabled = false;
    a->wave.enabled = false;
    a->noise.enabled = false;

    /* No charge on the output capacitor yet, so the first sample cannot click. */
    a->hp_l = a->hp_r = a->hp_cap_l = a->hp_cap_r = 0.0f;
}

uint8_t apu_read_reg(APU *a, uint8_t reg)
{
    /* NR52 is always readable: bit 7 is the power flag and bits 0-3 are the
     * live channel-status bits, so they are computed, never stored. */
    if (reg == 0x26u) {
        uint8_t v = (uint8_t)(0x70u | (a->power ? 0x80u : 0x00u));
        if (a->sq1.enabled)   v |= 0x01u;
        if (a->sq2.enabled)   v |= 0x02u;
        if (a->wave.enabled)  v |= 0x04u;
        if (a->noise.enabled) v |= 0x08u;
        return v;
    }

    /* Wave RAM is plain memory and stays readable even when powered off. */
    if (reg >= 0x30u && reg <= 0x3Fu)
        return a->wave_ram[(unsigned)(reg - 0x10u) & 0x0Fu];

    /* Everything else reads 0xFF while the unit is powered down. */
    if (!a->power) return 0xFFu;

    return nr_read(a, (unsigned)(reg - 0x10u));
}

void apu_write_reg(APU *a, uint8_t reg, uint8_t v)
{
    /* NR52 is the power switch and is reachable even while powered off. */
    if (reg == 0x26u) {
        if ((v & 0x80u) == 0u) {
            /* Powering off clears the whole register file and silences every
             * channel; wave RAM is deliberately NOT cleared. */
            memset(a->nr, 0, sizeof(a->nr));
            a->nr50 = 0u;
            a->nr51 = 0u;
            a->sq1.enabled = false;
            a->sq2.enabled = false;
            a->wave.enabled = false;
            a->noise.enabled = false;
            a->power = false;
        } else {
            a->power = true;
        }
        a->nr52 = (uint8_t)(v & 0x80u);   /* only bit 7 is writable */
        return;
    }

    /* While powered off only wave RAM still accepts writes on the DMG. Wave RAM
     * lives in wave_ram[], not in nr[]: nr[] is the 0x20-byte NR10..NR2F file,
     * so indexing it with (reg - 0x10) for reg >= 0x30 writes past its end and
     * clobbers nr50/nr51/nr52/power and the frame-sequencer state. */
    if (!a->power) {
        if (reg >= 0x30u && reg <= 0x3Fu)
            a->wave_ram[(unsigned)(reg - 0x10u) & 0x0Fu] = v;
        return;
    }

    /* Wave RAM (FF30-FF3F): two 4-bit samples per byte, low nibble first. */
    if (reg >= 0x30u && reg <= 0x3Fu) {
        a->wave_ram[(unsigned)(reg - 0x10u) & 0x0Fu] = v;
        return;
    }

    /* Every other register keeps its raw byte; the channel structs below hold
     * the decoded behaviour so the per-T-cycle path never re-reads bits. */
    a->nr[reg - 0x10u] = v;

    switch (reg) {
    case 0x10u:   /* NR10: sweep period, direction and shift */
        a->sq1.sweep_period = (v >> 4u) & 0x07u;
        a->sq1.sweep_negate = (v & 0x08u) != 0u;
        a->sq1.sweep_shift  = v & 0x07u;
        break;

    case 0x11u:   /* NR11: duty pattern + length load */
        a->sq1.duty = (uint8_t)((v >> 6u) & 0x03u);
        /* The length load only reaches the counter on a trigger. */
        break;

    case 0x12u: { /* NR12: envelope initial volume, direction, period */
        a->sq1.env_initial = (uint8_t)((v >> 4u) & 0x0Fu);
        a->sq1.env_dir     = (v & 0x08u) != 0u;
        a->sq1.env_period  = v & 0x07u;
        /* Bits 7-3 all zero turn the DAC off and kill the channel. */
        a->sq1.dac_enabled = (v & 0xF8u) != 0u;
        if (!a->sq1.dac_enabled) a->sq1.enabled = false;
        break;
    }

    case 0x13u:   /* NR13: period low 8 bits */
        a->sq1.period = (a->sq1.period & 0x700u) | v;
        break;

    case 0x14u: { /* NR14: period high, length enable, trigger */
        a->sq1.period = (a->sq1.period & 0x0FFu) | ((unsigned)(v & 0x07u) << 8u);
        a->sq1.length_enable = (v & 0x40u) != 0u;
        if (v & 0x80u) {
            square_trigger(a, &a->sq1, 0x01u);
            square_sweep_trigger(&a->sq1);
        }
        break;
    }

    case 0x16u:   /* NR21: duty pattern + length load */
        a->sq2.duty = (uint8_t)((v >> 6u) & 0x03u);
        break;

    case 0x17u: { /* NR22: envelope */
        a->sq2.env_initial = (uint8_t)((v >> 4u) & 0x0Fu);
        a->sq2.env_dir     = (v & 0x08u) != 0u;
        a->sq2.env_period  = v & 0x07u;
        a->sq2.dac_enabled = (v & 0xF8u) != 0u;
        if (!a->sq2.dac_enabled) a->sq2.enabled = false;
        break;
    }

    case 0x18u:   /* NR23: period low */
        a->sq2.period = (a->sq2.period & 0x700u) | v;
        break;

    case 0x19u: { /* NR24: period high, length enable, trigger */
        a->sq2.period = (a->sq2.period & 0x0FFu) | ((unsigned)(v & 0x07u) << 8u);
        a->sq2.length_enable = (v & 0x40u) != 0u;
        if (v & 0x80u) square_trigger(a, &a->sq2, 0x06u);
        break;
    }

    case 0x1Au: { /* NR30: wave DAC enable */
        a->wave.dac_enabled = (v & 0x80u) != 0u;
        if (!a->wave.dac_enabled) a->wave.enabled = false;
        break;
    }

    case 0x1Bu:   /* NR31: length load, copied on trigger */
        break;

    case 0x1Cu:   /* NR32: output volume code */
        a->wave.volume_code = (uint8_t)((v >> 5u) & 0x03u);
        break;

    case 0x1Du:   /* NR33: period low */
        a->wave.period = (a->wave.period & 0x700u) | v;
        break;

    case 0x1Eu: { /* NR34: period high, length enable, trigger */
        a->wave.period = (a->wave.period & 0x0FFu) | ((unsigned)(v & 0x07u) << 8u);
        a->wave.length_enable = (v & 0x40u) != 0u;
        if (v & 0x80u) wave_trigger(a);
        break;
    }

    case 0x20u:   /* NR41: length load, copied on trigger */
        break;

    case 0x21u: { /* NR42: envelope */
        a->noise.env_initial = (uint8_t)((v >> 4u) & 0x0Fu);
        a->noise.env_dir     = (v & 0x08u) != 0u;
        a->noise.env_period  = v & 0x07u;
        a->noise.dac_enabled = (v & 0xF8u) != 0u;
        if (!a->noise.dac_enabled) a->noise.enabled = false;
        break;
    }

    case 0x22u:   /* NR43: divisor, width mode, clock shift */
        a->noise.divisor_code = v & 0x07u;
        a->noise.width7       = (v & 0x08u) != 0u;
        a->noise.clock_shift  = (v >> 4u) & 0x0Fu;
        break;

    case 0x23u:   /* NR44: length enable + trigger */
        a->noise.length_enable = (v & 0x40u) != 0u;
        if (v & 0x80u) noise_trigger(a);
        break;

    case 0x24u:   /* NR50: master volume, bits 3 and 7 unused */
        a->nr50 = (uint8_t)(v & 0x77u);
        break;

    case 0x25u:   /* NR51: panning, every bit is used */
        a->nr51 = v;
        break;

    default:
        /* Unused registers (FF15-FF1F, FF27-FF2F): writes are dropped. */
        break;
    }
}

void apu_frame_step(APU *a)
{
    /* One 512 Hz step. The step is advanced first, so the sequence of clocks
     * below is 1,2,3,4,5,6,7,0. */
    a->fs_step = (a->fs_step + 1u) & 7u;

    /* Length counters run at 256 Hz: steps 0, 2, 4 and 6. */
    if ((a->fs_step & 1u) == 0u) {
        length_clock(a->sq1.length_enable, &a->sq1.length, &a->sq1.enabled);
        length_clock(a->sq2.length_enable, &a->sq2.length, &a->sq2.enabled);
        length_clock(a->wave.length_enable, &a->wave.length, &a->wave.enabled);
        length_clock(a->noise.length_enable, &a->noise.length, &a->noise.enabled);
    }

    /* Sweep runs at 128 Hz: steps 2 and 6. */
    if (a->fs_step == 2u || a->fs_step == 6u) {
        square_clock_sweep(&a->sq1);
    }

    /* Envelopes run at 64 Hz: step 7. The square channels' DAC output is
     * latched on this step too; the "zombie" behaviour is not modeled, so the
     * mixer reads the live volume instead of a latched copy. */
    if (a->fs_step == 7u) {
        env_clock(&a->sq1.volume, a->sq1.env_dir, a->sq1.env_period, &a->sq1.env_timer);
        env_clock(&a->sq2.volume, a->sq2.env_dir, a->sq2.env_period, &a->sq2.env_timer);
        env_clock(&a->noise.volume, a->noise.env_dir, a->noise.env_period, &a->noise.env_timer);
    }
}

/* --- Mixing ------------------------------------------------------------- */

/* square_output — the 4-bit DAC value of one square channel. */
static uint8_t square_output(const Square *s)
{
    if (!s->enabled || !s->dac_enabled) return 0u;
    return duty_table[s->duty][s->duty_pos] ? s->volume : 0u;
}

/* wave_output — the current wave-RAM sample, scaled by the volume code.
 * Code 0 mutes the channel; 1/2/3 give 100 %, 50 % and 25 %. */
static uint8_t wave_output(const APU *a, const Wave *w)
{
    if (!w->enabled || !w->dac_enabled) return 0u;
    if (w->volume_code == 0u) return 0u;
    /* Two 4-bit samples per byte: the low nibble is the even sample. */
    uint8_t byte = a->wave_ram[w->pos >> 1u];
    uint8_t sample = (w->pos & 1u) ? (uint8_t)(byte >> 4u)
                                  : (uint8_t)(byte & 0x0Fu);
    return (uint8_t)(sample >> (w->volume_code - 1u));
}

/* noise_output — the LFSR's low bit inverts the volume. */
static uint8_t noise_output(const Noise *n)
{
    if (!n->enabled || !n->dac_enabled) return 0u;
    return (n->lfsr & 1u) ? 0u : n->volume;
}

/* apu_emit_sample — mix the four channels into one stereo frame and push it
 * into the ring buffer. Called at SAMPLE_RATE, not per T-cycle. */
static void apu_emit_sample(APU *a)
{
    uint8_t ch[4];
    ch[0] = square_output(&a->sq1);
    ch[1] = square_output(&a->sq2);
    ch[2] = wave_output(a, &a->wave);
    ch[3] = noise_output(&a->noise);

    /* NR51 routes each channel to the left and/or right output. */
    unsigned right = 0u, left = 0u;
    for (unsigned i = 0u; i < 4u; i++) {
        /* A muted channel is silenced here only; its timers keep running, so
         * muting never changes the machine's behaviour. */
        if (a->mute & (1u << i)) ch[i] = 0u;
        if (a->nr51 & (1u << i))        right += ch[i];   /* bits 0-3 */
        if (a->nr51 & (0x10u << i))     left  += ch[i];   /* bits 4-7 */
    }

    /* Four DACs of 0-15 sum to at most 60; scale to [-1, 1] and apply the
     * master volume, one control per side. */
    float in_l = (float)left  / 60.0f * (float)(((a->nr50 >> 4u) & 0x07u) + 1u) / 8.0f;
    float in_r = (float)right / 60.0f * (float)((a->nr50 & 0x07u) + 1u) / 8.0f;

    /* DC blocker: the DACs sit at a constant offset, so the mix is passed
     * through a one-pole high-pass (the DMG's output capacitor). Without it,
     * every channel start would produce an audible thump. */
    float out_l = in_l - a->hp_cap_l + 0.996f * a->hp_l;
    float out_r = in_r - a->hp_cap_r + 0.996f * a->hp_r;
    a->hp_cap_l = in_l;   /* "last input" for the next sample */
    a->hp_cap_r = in_r;
    a->hp_l = out_l;      /* "last output" for the next sample */
    a->hp_r = out_r;

    /* Clip before converting to 16-bit, otherwise a loud mix wraps around. */
    if (out_l >  1.0f) out_l =  1.0f;
    if (out_l < -1.0f) out_l = -1.0f;
    if (out_r >  1.0f) out_r =  1.0f;
    if (out_r < -1.0f) out_r = -1.0f;
    int16_t sl = (int16_t)(out_l * 32767.0f);
    int16_t sr = (int16_t)(out_r * 32767.0f);

    /* Push the frame if there is room; if the host is behind, drop it and
     * count it. Blocking the emulation thread here is never acceptable. */
    unsigned next = (a->head + 1u) & (APU_BUF_FRAMES - 1u);
    if (next == a->tail) {
        /* Buffer full: drop the frame and count it. The counter is never
         * reset, so logging only on the first drop reports at most once. */
        if (a->underruns == 0u)
            gb_log("apu: ring buffer full, dropping audio frames");
        a->underruns++;
        return;
    }
    a->buf[a->head * 2u]      = sl;
    a->buf[a->head * 2u + 1u]  = sr;
    a->head = next;
}

void apu_tick(APU *a, MMU *m, uint32_t t_cycles)
{
    (void)m;   /* the APU owns no bus state; the MMU is here for the signature */

    for (uint32_t i = 0u; i < t_cycles; i++) {
        /* Each channel is an independent counter, so they advance per T-cycle. */
        if (a->power) {
            if (a->sq1.enabled)   square_tick(&a->sq1);
            if (a->sq2.enabled)   square_tick(&a->sq2);
            if (a->wave.enabled)  wave_tick(&a->wave);
            if (a->noise.enabled) noise_tick(&a->noise);
        }

        /* Integer resampling: adding SAMPLE_RATE per T-cycle and emitting
         * whenever the accumulator passes the CPU clock gives exactly
         * SAMPLE_RATE samples per emulated second, with no drift. The
         * threshold is the CPU clock, because apu_tick() is given CPU
         * T-cycles: using the APU clock here would emit four times too
         * many samples. */
        a->sample_acc += (uint32_t)a->sample_rate;
        while (a->sample_acc >= CPU_CLOCK_HZ) {
            a->sample_acc -= CPU_CLOCK_HZ;
            apu_emit_sample(a);
        }
    }
}

/* --- Host interface ----------------------------------------------------- */

size_t apu_drain(APU *a, int16_t *out, size_t max_frames)
{
    if (out == NULL || max_frames == 0u) return 0u;
    size_t n = 0u;
    while (n < max_frames && a->tail != a->head) {
        out[n * 2u]     = a->buf[a->tail * 2u];
        out[n * 2u + 1u] = a->buf[a->tail * 2u + 1u];
        a->tail = (a->tail + 1u) & (APU_BUF_FRAMES - 1u);
        n++;
    }
    return n;
}

int apu_sample_rate(const APU *a)
{
    return a->sample_rate;
}

void apu_set_mute(APU *a, unsigned mask)
{
    a->mute = mask & 0x0Fu;
}

void apu_set_sample_rate(APU *a, int rate)
{
    a->sample_rate = rate;
}

/* --- Save states -------------------------------------------------------- */

void apu_serialize(APU *a, FILE *f)
{
    GB_SER(a->sq1, f);
    GB_SER(a->sq2, f);
    GB_SER(a->wave, f);
    GB_SER(a->noise, f);
    GB_SER(a->wave_ram, f);
    GB_SER(a->nr, f);
    GB_SER(a->nr50, f);
    GB_SER(a->nr51, f);
    GB_SER(a->nr52, f);
    GB_SER(a->power, f);
    GB_SER(a->fs_div, f);
    GB_SER(a->fs_step, f);
    GB_SER(a->sample_acc, f);
    GB_SER(a->hp_l, f);
    GB_SER(a->hp_r, f);
    GB_SER(a->hp_cap_l, f);
    GB_SER(a->hp_cap_r, f);
}

void apu_deserialize(APU *a, FILE *f)
{
    GB_DESER(a->sq1, f);
    GB_DESER(a->sq2, f);
    GB_DESER(a->wave, f);
    GB_DESER(a->noise, f);
    GB_DESER(a->wave_ram, f);
    GB_DESER(a->nr, f);
    GB_DESER(a->nr50, f);
    GB_DESER(a->nr51, f);
    GB_DESER(a->nr52, f);
    GB_DESER(a->power, f);
    GB_DESER(a->fs_div, f);
    GB_DESER(a->fs_step, f);
    GB_DESER(a->sample_acc, f);
    GB_DESER(a->hp_l, f);
    GB_DESER(a->hp_r, f);
    GB_DESER(a->hp_cap_l, f);
    GB_DESER(a->hp_cap_r, f);

    /* The ring buffer contents are the host's; a restored state starts with an
     * empty buffer rather than stale samples from the previous run. */
    a->head = 0u;
    a->tail = 0u;
}
