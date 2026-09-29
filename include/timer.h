/* timer.h — DIV, TIMA, TMA, TAC (L9).
 *
 * All four registers are views onto ONE 16-bit free-running counter:
 *   - DIV (FF04) reads the counter's high byte (ticking at 16384 Hz),
 *   - TIMA (FF05) increments when the bit selected by TAC falls 1 -> 0,
 *   - TAC (FF07) selects that bit and can enable/disable the timer,
 *   - and the APU frame sequencer is clocked by a different divider bit.
 * Modeling them as one counter is what makes Blargg's mem_timing pass.
 */
#ifndef GB_TIMER_H
#define GB_TIMER_H

#include "gbdefs.h"

typedef struct MMU MMU;

typedef struct {
    uint16_t div_counter;      /* the free-running 16-bit counter */
    uint8_t  tima;             /* timer counter */
    uint8_t  tma;              /* timer modulo (reload value) */
    uint8_t  tac;              /* timer control */
    uint8_t  overflow_delay;   /* T-cycles left before TIMA reload + IF set */
    bool     overflow_reload;  /* true when the pending step must reload TIMA */
} Timer;

void timer_init(Timer *t);
uint8_t timer_read(Timer *t, uint16_t addr);
void timer_write(Timer *t, uint16_t addr, uint8_t v);

/* timer_tick — advance by t_cycles T-cycles, raising the timer interrupt
 * through mmu_if_set() when TIMA overflows. The APU frame sequencer is clocked
 * here as well: the divider's bit 12 falling edge (once every 8192 T-cycles)
 * calls apu_frame_step(), so the APU and DIV share one counter, as on hardware. */
void timer_tick(Timer *t, uint32_t t_cycles, MMU *m);

void timer_serialize(Timer *t, FILE *f);
void timer_deserialize(Timer *t, FILE *f);

#endif /* GB_TIMER_H */
