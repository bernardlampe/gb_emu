/* joypad.h — the P1/JOYP input matrix (L23).
 *
 * FF00 is not a register you store: it is a multiplexer. Bits 4-5 (active low)
 * select whether the low nibble exposes directions or buttons, and the low nibble
 * reads those lines active-low. The host pushes the current SDL state in with
 * joypad_set_input(); the game reads whenever it likes.
 */
#ifndef GB_JOYPAD_H
#define GB_JOYPAD_H

#include "gbdefs.h"

typedef struct MMU MMU;

/* Button bits used by joypad_set_input() and by the host's key mapping.
 *
 * Each group is a separate mask, and the bit values match the positions the
 * game reads in JOYP's low nibble, so the host's mapping is the hardware's
 * mapping: bit 0 of the d-pad mask is Right, bit 0 of the button mask is A.
 */
#define JOY_RIGHT  (1u << 0)
#define JOY_LEFT   (1u << 1)
#define JOY_UP     (1u << 2)
#define JOY_DOWN   (1u << 3)

#define JOY_A      (1u << 0)
#define JOY_B      (1u << 1)
#define JOY_SELECT (1u << 2)
#define JOY_START  (1u << 3)

typedef struct {
    uint8_t select;    /* last value written to FF00, bits 4-5 kept */
    uint8_t dpad;      /* bit set = line held low = pressed */
    uint8_t buttons;   /* bit set = line held low = pressed */
} Joypad;

void    joypad_init(Joypad *j);
void    joypad_write(Joypad *j, uint8_t v);
uint8_t joypad_read(Joypad *j);

/* joypad_set_input — called by the host once per frame with the current
 * button state. Raises the joypad interrupt on a high-to-low transition of any
 * line that is currently selected, which is what real hardware does. */
void joypad_set_input(Joypad *j, uint8_t buttons, uint8_t dpad, MMU *m);

void joypad_serialize(Joypad *j, FILE *f);
void joypad_deserialize(Joypad *j, FILE *f);

#endif /* GB_JOYPAD_H */
