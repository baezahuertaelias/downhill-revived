/* Controllers on the Switch: gamepad.c's job over libnx's pads. Player 1 is the handheld or first
   Joy-Con pair or Pro Controller. A state is gamepad.c's GamepadRaw, which the player's layout turns
   into the PS2 pad's (gamepad_layout.c); with the default layout, Nintendo's way round for confirm
   and back: A reads as the bottom input (Cross) and B as the right one (Circle); the others by
   position, left (Y) Square and top (X) Triangle. So the kind is PAD_KIND_NONE: the PS2's symbols,
   not Nintendo letters that would name B for Cross. All of libnx's calls are on the main thread
   (nx_host.c), which starts the pads before the game's thread; libnx's start has made hid ready.
   The game's thread only copies states and sets wishes. No keyboard events come here, so
   keyboard_focused stays 0 and host_pad_device's keyboard adds nothing. */
#include "hle.h"
#include "controls_table.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

enum { SLOTS = 4 };
static PadState pads[SLOTS];
static GamepadRaw latest[SLOTS];   /* under lock */
static uint32_t held_since[SLOTS];  /* buttons held since the game's last read (gamepad_raw_take), under lock */
static int present[SLOTS];
static Mutex lock;
/* A motor's handle depends on the style in use: each set is activated when that style first rumbles. */
enum { RUMBLE_FULLKEY, RUMBLE_JOYDUAL, RUMBLE_JOYLEFT, RUMBLE_JOYRIGHT, RUMBLE_HANDHELD, RUMBLE_STYLES };
static const struct { u32 style; int motors; } rumble_styles[RUMBLE_STYLES] = {
    {HidNpadStyleTag_NpadFullKey, 2}, {HidNpadStyleTag_NpadJoyDual, 2}, {HidNpadStyleTag_NpadJoyLeft, 1},
    {HidNpadStyleTag_NpadJoyRight, 1}, {HidNpadStyleTag_NpadHandheld, 2},
};
static HidVibrationDeviceHandle rumble_handles[SLOTS][RUMBLE_STYLES][2];
static signed char rumble_state[SLOTS][RUMBLE_STYLES];  /* 0 not asked yet, 1 ready, -1 refused */
static int want_light[SLOTS], want_heavy[SLOTS], sent_light[SLOTS], sent_heavy[SLOTS];
static int motors_held;  /* the game is away or ending (gamepad_nx_hold) */

/* libnx's stick value (+-32767, up positive) as SDL's axis (down positive) */
static int16_t axis_value(int v, int invert)
{
    int x = invert ? -v : v;
    return (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
}

int gamepad_thread_start(void)
{
    padConfigureInput(SLOTS, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pads[0]);  /* No1 and the handheld together */
    for (int i = 1; i < SLOTS; i++) padInitialize(&pads[i], (HidNpadIdType)(HidNpadIdType_No1 + i));
    for (int i = 0; i < SLOTS; i++) sent_light[i] = sent_heavy[i] = -1;
    gamepad_thread_update();  /* the pads there at the start have a state */
    return 1;
}
void gamepad_device(int change, unsigned id) { (void)change; (void)id; }

static void rumble_send(int i, int kind, HidNpadIdType id, float amplitude)
{
    if (!rumble_state[i][kind]) {
        Result rc = hidInitializeVibrationDevices(rumble_handles[i][kind], rumble_styles[kind].motors, id, rumble_styles[kind].style);
        rumble_state[i][kind] = R_SUCCEEDED(rc) ? 1 : -1;
        if (R_FAILED(rc)) fprintf(stderr, "controller: player %d has no vibration (style %x): 0x%x\n", i + 1, rumble_styles[kind].style, rc);
    }
    if (rumble_state[i][kind] < 0) return;
    HidVibrationValue v = {amplitude, 160.0f, amplitude, 320.0f}, both[2] = {v, v};
    hidSendVibrationValues(rumble_handles[i][kind], both, rumble_styles[kind].motors);
}

/* Every controller the slot reads now: player 1 may be the handheld and a No1 controller at once. */
static void rumble_all(int i, float amplitude)
{
    if (i == 0 && padIsHandheld(&pads[0])) rumble_send(i, RUMBLE_HANDHELD, HidNpadIdType_Handheld, amplitude);
    HidNpadIdType id = (HidNpadIdType)(HidNpadIdType_No1 + i);
    if (!padIsNpadActive(&pads[i], id)) return;
    u32 style = hidGetNpadStyleSet(id);
    for (int kind = RUMBLE_FULLKEY; kind <= RUMBLE_JOYRIGHT; kind++)
        if (style & rumble_styles[kind].style) { rumble_send(i, kind, id, amplitude); break; }
}

static void rumble(int i)
{
    int light = __atomic_load_n(&want_light[i], __ATOMIC_RELAXED), heavy = __atomic_load_n(&want_heavy[i], __ATOMIC_RELAXED);
    if (light == sent_light[i] && heavy == sent_heavy[i]) return;
    sent_light[i] = light, sent_heavy[i] = heavy;
    rumble_all(i, heavy / 255.0f > (light ? 0.4f : 0.0f) ? heavy / 255.0f : (light ? 0.4f : 0.0f));
}

/* Main thread: the motors stop while the game is away or ending (they keep the last command
   otherwise), and take up the game's wish again afterwards. */
void gamepad_nx_hold(int hold)
{
    motors_held = hold;
    for (int i = 0; i < SLOTS; i++) {
        if (hold && padIsConnected(&pads[i])) rumble_all(i, 0.0f);
        sent_light[i] = sent_heavy[i] = -1;
    }
}

void gamepad_thread_update(void)
{
    for (int i = 0; i < SLOTS; i++) {
        padUpdate(&pads[i]);
        GamepadRaw p;
        memset(&p, 0, sizeof p);
        int on = padIsConnected(&pads[i]);
        u64 b = padGetButtons(&pads[i]);
        HidAnalogStickState l = padGetStickPos(&pads[i], 0), r = padGetStickPos(&pads[i], 1);
        static const struct { u64 from; const char *input; } map[] = {
            {HidNpadButton_A, "south"}, {HidNpadButton_B, "east"}, {HidNpadButton_Y, "west"}, {HidNpadButton_X, "north"},
            {HidNpadButton_Minus, "back"}, {HidNpadButton_Plus, "start"}, {HidNpadButton_StickL, "ls"}, {HidNpadButton_StickR, "rs"},
            {HidNpadButton_L, "lb"}, {HidNpadButton_R, "rb"},
            {HidNpadButton_Up, "dpad_up"}, {HidNpadButton_Down, "dpad_down"}, {HidNpadButton_Left, "dpad_left"}, {HidNpadButton_Right, "dpad_right"},
        };
        for (size_t k = 0; k < sizeof map / sizeof *map; k++)
            if (b & map[k].from) p.buttons |= 1u << gamepad_input_id(map[k].input);
        p.trigger[0] = b & HidNpadButton_ZL ? 255 : 0;  /* ZL and ZR are buttons: fully pressed or not */
        p.trigger[1] = b & HidNpadButton_ZR ? 255 : 0;
        p.axis[0] = axis_value(l.x, 0), p.axis[1] = axis_value(l.y, 1);
        p.axis[2] = axis_value(r.x, 0), p.axis[3] = axis_value(r.y, 1);
        mutexLock(&lock);
        latest[i] = p;
        held_since[i] |= p.buttons;
        present[i] = on;
        mutexUnlock(&lock);
        if (!on) sent_light[i] = sent_heavy[i] = -1;  /* one that comes back is told again */
        else if (!motors_held) rumble(i);
    }
}

int gamepad_start(void) { return 1; }
/* Players keep libnx's order; no first-press claim (gamepad.c's, finding 232), so nothing to publish. */
void gamepad_set_multiplayer(int local_multiplayer) { (void)local_multiplayer; }

static int read_raw(int player, GamepadRaw *out, int take)
{
    if (player < 0 || player >= SLOTS) return 0;
    mutexLock(&lock);
    int ok = present[player];
    if (ok) *out = latest[player];
    if (ok && take) out->buttons |= held_since[player];
    if (take) held_since[player] = 0;
    mutexUnlock(&lock);
    return ok;
}
int gamepad_raw(int player, GamepadRaw *out) { return read_raw(player, out, 0); }
int gamepad_raw_take(int player, GamepadRaw *out) { return read_raw(player, out, 1); }
int gamepad_kind(int player) { (void)player; return PAD_KIND_NONE; }
void gamepad_vibrate(int player, int light, int heavy)
{
    if (player < 0 || player >= SLOTS) return;
    __atomic_store_n(&want_light[player], light ? 1 : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&want_heavy[player], heavy & 255, __ATOMIC_RELAXED);
}
