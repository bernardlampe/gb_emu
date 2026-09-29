/* joypad.c — the FF00 input multiplexer (L23).
 *
 * FF00 is not storage: it is a multiplexer. Bits 4-5 (active low) select whether
 * the low nibble exposes directions or buttons, and the low nibble reads those
 * lines active-low, so 0 means "held" and 1 means "released". A game writes
 * 0x20 to read the buttons, then polls until the bit it wants goes low.
 *
 * Reference implementation for CS 4XX "System Emulation".
 */
#include "joypad.h"

#include "mmu.h"

void joypad_init(Joypad *j)
{
    /* Post-boot P1 reads 0xCF: both groups selected and nothing held. */
    j->select = 0x00u;
    j->dpad = 0;
    j->buttons = 0;
}

/* joypad_value — compute FF00 from the current selection and input state.
 *
 * The masks in j->dpad and j->buttons are "held" masks: a set bit means the
 * key is down, which reads back as 0 in the low nibble.
 */
static uint8_t joypad_value(const Joypad *j)
{
    uint8_t held = 0;

    /* An unselected group drives nothing, so its lines read 1. */
    if (!(j->select & 0x20u)) held |= (uint8_t)(j->buttons & 0x0Fu);
    if (!(j->select & 0x10u)) held |= (uint8_t)(j->dpad & 0x0Fu);

    return (uint8_t)(0xC0u | j->select | ((uint8_t)(~held) & 0x0Fu));
}

uint8_t joypad_read(Joypad *j)
{
    /* Recompute on every read: the game may poll FF00 at any time, and the
     * answer must reflect the CURRENT input, not a latched event. */
    return joypad_value(j);
}

void joypad_write(Joypad *j, uint8_t v)
{
    j->select = (uint8_t)(v & 0x30u);
}

void joypad_set_input(Joypad *j, uint8_t buttons, uint8_t dpad, MMU *m)
{
    /* The joypad interrupt fires on a HIGH-TO-LOW transition of a line that is
     * currently selected, i.e. when a key becomes held while the game is reading
     * that group. Comparing the previous and current read values is what makes
     * this exact. */
    uint8_t old_value = joypad_value(j);

    j->buttons = (uint8_t)(buttons & 0x0Fu);
    j->dpad = (uint8_t)(dpad & 0x0Fu);

    uint8_t new_value = joypad_value(j);

    /* A line that was 1 (released) and is now 0 (held) is a falling edge. */
    if ((old_value & (uint8_t)~new_value) & 0x0Fu) mmu_if_set(m, IF_JOYPAD);
}

void joypad_serialize(Joypad *j, FILE *f)
{
    GB_SER(j->select, f); GB_SER(j->dpad, f); GB_SER(j->buttons, f);
}

void joypad_deserialize(Joypad *j, FILE *f)
{
    GB_DESER(j->select, f); GB_DESER(j->dpad, f); GB_DESER(j->buttons, f);
}
