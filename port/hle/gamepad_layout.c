/* A controller's state (GamepadRaw) through a player's layout into the PS2 pad's (finding 157):
   the part of the controllers that needs no SDL, shared by gamepad.c (SDL3) and the Switch's
   gamepad_nx.c. See gamepad.c's top for the layout's rules. */
#include "hle.h"
#include "stick.h"
#include "controls_table.h"

#include <string.h>

enum { PRESSED = 30 };  /* a trigger's byte past PRESSED is a press of its button */

static const uint16_t pressure_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                            PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};

int gamepad_input_id(const char *name)
{
    for (int i = 0; i < CONTROL_PAD_INPUTS; i++)
        if (!strcmp(control_pad_inputs[i].name, name)) return control_pad_inputs[i].id;
    return 0;
}

/* The input whose state stands for `id`'s place with the layout's shoulder swaps (finding 189):
   with Swap L1/L2 the left shoulder (lb) reads the left trigger (lt) and the other way round, with
   Swap R1/R2 the right ones. A swap is its own inverse, so the same call turns a pressed input into
   its place (the CONTROLS page's wait, host.c). */
int gamepad_place(const struct ControlsLayout *layout, int id)
{
    static int lb, lt, rb, rt;
    if (!lb) lt = gamepad_input_id("lt"), rb = gamepad_input_id("rb"), rt = gamepad_input_id("rt"), lb = gamepad_input_id("lb");
    if (layout->swap_shoulders & CONTROL_SWAP_L) {
        if (id == lb) return lt;
        if (id == lt) return lb;
    }
    if (layout->swap_shoulders & CONTROL_SWAP_R) {
        if (id == rb) return rt;
        if (id == rt) return rb;
    }
    return id;
}

/* An input's value in a state: a button's 255 while held, a trigger's byte. */
static int input_value(const GamepadRaw *raw, int id)
{
    for (int i = 0; i < CONTROL_PAD_INPUTS; i++)
        if (control_pad_inputs[i].id == id && control_pad_inputs[i].trigger) return raw->trigger[control_pad_inputs[i].trigger - 1];
    return id > 0 && id < 32 && raw->buttons & 1u << id ? 255 : 0;
}

/* An axis turned round: -32768 and 32767 change places, the centre stays. */
static int16_t inverted(int16_t v) { return v == -32768 ? 32767 : (int16_t)-v; }

void gamepad_apply(const struct ControlsLayout *layout, const GamepadRaw *raw, int dead_zone, HostPad *out)
{
    uint8_t pressure[CONTROL_BUTTONS] = {0};
    for (int t = 0; t < CONTROL_BUTTONS; t++)
        for (int s = 0; s < CONTROL_SLOTS; s++) {
            int value = layout->input[t][s] ? input_value(raw, gamepad_place(layout, layout->input[t][s])) : 0;
            if (value > pressure[t]) pressure[t] = (uint8_t)value;
            if (value > PRESSED) out->buttons |= control_targets[t].bit;
        }
    for (int i = 0; i < 12; i++)
        for (int t = 0; t < CONTROL_BUTTONS; t++)
            if (control_targets[t].bit == pressure_order[i]) out->pressure[i] = pressure[t];
    int16_t axis[4];
    static const uint8_t invert[4] = {CONTROL_INVERT_LX, CONTROL_INVERT_LY, CONTROL_INVERT_RX, CONTROL_INVERT_RY};
    for (int i = 0; i < 4; i++) axis[i] = layout->invert & invert[i] ? inverted(raw->axis[i]) : raw->axis[i];
    int left = layout->swap_sticks ? 2 : 0, right = 2 - left;
    out->left_x = stick_byte(axis[left], dead_zone), out->left_y = stick_byte(axis[left + 1], dead_zone);
    out->right_x = stick_byte(axis[right], dead_zone), out->right_y = stick_byte(axis[right + 1], dead_zone);
}
