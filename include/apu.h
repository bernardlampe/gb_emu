/* apu.h — the four-channel sound unit (L24-L27).
 *
 * The APU is clocked at CPU/4 = 1.048576 MHz and produces one stereo sample
 * every APU_CLOCK_HZ / SAMPLE_RATE cycles. The frame sequencer (length,
 * sweep, envelope) runs at 512 Hz, driven by the timer's divider so that it
 * shares the machine's clock. The host drains the ring buffer and hands the
 * samples to SDL; the core never includes SDL.
 */
#ifndef GB_APU_H
#define GB_APU_H

#include "gbdefs.h"

typedef struct MMU MMU;

/* One square channel (NR1x or NR2x). */
typedef struct {
    bool     enabled;        /* channel status, exposed in NR52 */
    bool     dac_enabled;    /* NRx2 bits 7-3 must be non-zero */
    uint8_t  duty;           /* 0-3: 12.5 / 25 / 50 / 75 percent */
    uint8_t  duty_pos;       /* 0-7 position within the 8-step duty */
    unsigned period;         /* 11-bit frequency period, 2048 - freq */
    unsigned timer;          /* T-cycles until the next duty step */

    uint8_t  volume;          /* current envelope volume, 0-15 */
    uint8_t  env_initial;
    bool     env_dir;         /* true = increase */
    unsigned env_period;
    unsigned env_timer;

    bool     length_enable;
    unsigned length;          /* 0-63, ticks at 256 Hz */

    /* Sweep (square 1 only) */
    unsigned sweep_period;
    unsigned sweep_shift;
    bool     sweep_negate;
    unsigned sweep_timer;
    bool     sweep_enabled;
    bool     sweep_neg_used;   /* the obscure "negate overflow" rule needs this */
} Square;

typedef struct {
    bool     enabled;
    bool     dac_enabled;      /* NR30 bit 7 */
    unsigned period;
    unsigned timer;
    unsigned pos;              /* 0-31 sample position in wave RAM */
    uint8_t  volume_code;      /* 0 = mute, 1 = 100 %, 2 = 50 %, 3 = 25 % */
    bool     length_enable;
    unsigned length;
} Wave;

typedef struct {
    bool     enabled;
    bool     dac_enabled;
    unsigned divisor_code;     /* NR43 bits 0-2 */
    unsigned clock_shift;      /* NR43 bits 4-7 */
    unsigned timer;
    uint16_t lfsr;             /* 15-bit shift register */
    bool     width7;            /* true = 7-bit mode, taps bit 6 */
    uint8_t  volume;
    uint8_t  env_initial;
    bool     env_dir;
    unsigned env_period;
    unsigned env_timer;
    bool     length_enable;
    unsigned length;
} Noise;

typedef struct {
    Square sq1, sq2;
    Wave   wave;
    Noise  noise;
    uint8_t wave_ram[16];      /* 32 samples of 4 bits, stored 2 per byte */

    /* Raw NR10..NR2F register file, index = reg - 0x10. Reads of the audio
     * registers return these bytes, so writes that only matter to software (the
     * length loads, for instance) survive without a decoded field. */
    uint8_t nr[0x20];

    uint8_t nr50, nr51;         /* master volume and panning */
    uint8_t nr52;               /* bit 7 power, bits 0-3 channel status */
    bool    power;

    unsigned fs_div;             /* T-cycle divider for the 512 Hz sequencer */
    unsigned fs_step;            /* 0-7 step within the frame sequencer */

    /* Downsampling: add SAMPLE_RATE per T-cycle, emit whenever the
     * accumulator passes APU_CLOCK_HZ. Integer arithmetic, no drift. */
    uint32_t sample_acc;

    /* The host's device rate; apu_tick accumulates this per T-cycle so the
     * emulated audio clock matches the video clock exactly. */
    int      sample_rate;

    /* Ring buffer of interleaved stereo frames, drained by the host. */
    int16_t  buf[APU_BUF_FRAMES * 2];
    unsigned head, tail;
    uint32_t underruns;         /* emitted frames dropped because the buffer was full */

    /* The DMG's output capacitor: a high-pass filter that removes the DC
     * offset produced by the DACs. Without it, starting a channel clicks. */
    float hp_l, hp_r, hp_cap_l, hp_cap_r;

    /* Channels the host asked to silence for diagnosis (apu_set_mute). */
    unsigned mute;
} APU;

void    apu_init(APU *a);
void    apu_reset(APU *a);
uint8_t apu_read_reg(APU *a, uint8_t reg);
void    apu_write_reg(APU *a, uint8_t reg, uint8_t v);

/* apu_tick — advance channels and channel timers by t_cycles, emitting
 * downsampled stereo samples into the ring buffer. */
void apu_tick(APU *a, MMU *m, uint32_t t_cycles);

/* apu_frame_step — one 512 Hz frame-sequencer step: length counters, sweep
 * and envelope clocks. Called by the timer on the divider's 512 Hz edge. */
void apu_frame_step(APU *a);

/* apu_set_mute — silence channels for diagnosis only.
 *
 * Bit N of `mask` silences channel N+1 in the mixer. This is NOT hardware:
 * it exists so a student can isolate which channel is producing a sound they did
 * not expect (for example a noise channel that never stops). The channel itself
 * keeps running, so nothing else about the machine changes. */
void apu_set_mute(APU *a, unsigned mask);

/* apu_drain — copy up to max_frames stereo frames out of the ring buffer.
 * Returns the number of frames copied. Called by the host. */
size_t apu_drain(APU *a, int16_t *out, size_t max_frames);

/* apu_sample_rate / apu_set_sample_rate — the host's device rate. */
int  apu_sample_rate(const APU *a);
void apu_set_sample_rate(APU *a, int rate);

void apu_serialize(APU *a, FILE *f);
void apu_deserialize(APU *a, FILE *f);

#endif /* GB_APU_H */
