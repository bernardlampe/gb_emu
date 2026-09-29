/* timer.c — DIV, TIMA, TMA, TAC and the APU frame-sequencer clock (L9).
 *
 * One 16-bit counter serves four purposes, exactly as on hardware:
 *   - DIV (FF04)        = bits 8-15 of the counter (16384 Hz)
 *   - TIMA (FF05)        increments on a falling edge of the bit TAC selects
 *   - the APU frame sequencer steps on a falling edge of bit 12 (512 Hz)
 * Keeping one counter is what makes Blargg's mem_timing and Mooneye's timing
 * tests agree with each other; separate counters always drift apart.
 *
 * Reference implementation for CS 4XX "System Emulation".
 */
#include "timer.h"

#include "apu.h"
#include "mmu.h"

/* Which counter bit clocks TIMA for each TAC value.
 * 00 -> bit 9  (4096 Hz), 01 -> bit 3 (262144 Hz),
 * 10 -> bit 5  (65536 Hz), 11 -> bit 7 (16384 Hz).
 * Note that TAC rate 00 means "stopped", not "slowest". */
static const uint8_t TAC_BIT[4] = { 9, 3, 5, 7 };

void timer_init(Timer *t)
{
    t->div_counter = 0;
    t->tima = 0;
    t->tma = 0;
    t->tac = 0;
    t->overflow_delay = 0;
    t->overflow_reload = false;
}

uint8_t timer_read(Timer *t, uint16_t addr)
{
    switch (addr) {
    case 0xFF04: return (uint8_t)(t->div_counter >> 8);
    case 0xFF05: return t->tima;
    case 0xFF06: return t->tma;
    default: return (uint8_t)(t->tac | 0xF8u);
    }
}

void timer_write(Timer *t, uint16_t addr, uint8_t v)
{
    switch (addr) {
    case 0xFF04:
        /* Writing DIV resets the WHOLE 16-bit counter, which is why a game
         * can use it as a crude random number source. */
        t->div_counter = 0;
        break;
    case 0xFF05:
        /* Writing TIMA during the reload delay cancels the reload. */
        t->tima = v;
        t->overflow_delay = 0;
        t->overflow_reload = false;
        break;
    case 0xFF06:
        /* Writing TMA during the delay reloads TIMA with the NEW value. */
        t->tma = v;
        if (t->overflow_delay > 0 && t->overflow_reload) t->tima = v;
        break;
    default:
        t->tac = (uint8_t)(v & 0x07u);
        break;
    }
}

void timer_tick(Timer *t, uint32_t t_cycles, MMU *m)
{
    for (uint32_t i = 0; i < t_cycles; i++) {
        uint16_t before = t->div_counter;
        t->div_counter++;

        /* The APU frame sequencer is clocked by the falling edge of bit 12,
         * i.e. once every 8192 T-cycles (512 Hz). */
        if (((before >> 12) & 1u) && !((t->div_counter >> 12) & 1u))
            apu_frame_step(&m->apu);

        /* TIMA increments on a falling edge of the selected bit, not on every
         * tick. That detail is what Blargg's instr_timing measures. */
        if (t->tac & 0x04u) {
            uint8_t bit = TAC_BIT[t->tac & 0x03u];
            if (((before >> bit) & 1u) && !((t->div_counter >> bit) & 1u)) {
                if (t->tima == 0xFFu) {
                    /* TIMA overflows: it reloads from TMA four T-cycles
                     * later, and the interrupt is raised then too. */
                    t->tima = 0;
                    t->overflow_delay = 4;
                    t->overflow_reload = true;
                } else {
                    t->tima++;
                }
            }
        }

        if (t->overflow_delay > 0 && --t->overflow_delay == 0) {
            t->tima = t->tma;
            t->overflow_reload = false;
            mmu_if_set(m, IF_TIMER);
        }
    }
}

void timer_serialize(Timer *t, FILE *f)
{
    GB_SER(t->div_counter, f);
    GB_SER(t->tima, f); GB_SER(t->tma, f); GB_SER(t->tac, f);
    GB_SER(t->overflow_delay, f); GB_SER(t->overflow_reload, f);
}

void timer_deserialize(Timer *t, FILE *f)
{
    GB_DESER(t->div_counter, f);
    GB_DESER(t->tima, f); GB_DESER(t->tma, f); GB_DESER(t->tac, f);
    GB_DESER(t->overflow_delay, f); GB_DESER(t->overflow_reload, f);
}
