/* Button mapping: one keyboard key and one gamepad input per action (controls.h). Gamepad inputs are SDL
 * gamepad buttons, or the two triggers (PAD_LT, PAD_RT), which SDL reports as axes. The defaults map the DS face
 * buttons by position: A is the right button, as on Nintendo controllers. controls.ini, next to eggoftime.ini,
 * holds the mapping as "key.<action> = <SDL scancode name>" and "pad.<action> = <SDL gamepad button or axis
 * name>" lines ("none": unbound). */
#include <stdio.h>
#include <string.h>

#include "controls.h"
#include "rt.h"

#define PAD_LT SDL_GAMEPAD_BUTTON_COUNT
#define PAD_RT (SDL_GAMEPAD_BUTTON_COUNT + 1)
#define PAD_NONE (-1)
#define AXIS_PRESSED 16000 /* stick and trigger travel that counts as a press */
#define CAPTURE_MS 5000

typedef struct Binding { SDL_Scancode key; int pad; } Binding;

static const struct { const char *name, *ini; uint32_t ds; } actions[CTL_COUNT] = {
    [CTL_A] = {"A", "a", KEY_A},
    [CTL_B] = {"B", "b", KEY_B},
    [CTL_X] = {"X", "x", KEY_X},
    [CTL_Y] = {"Y", "y", KEY_Y},
    [CTL_L] = {"L", "l", KEY_L},
    [CTL_R] = {"R", "r", KEY_R},
    [CTL_START] = {"START", "start", KEY_START},
    [CTL_SELECT] = {"SELECT", "select", KEY_SELECT},
    [CTL_UP] = {"Up", "up", KEY_UP},
    [CTL_DOWN] = {"Down", "down", KEY_DOWN},
    [CTL_LEFT] = {"Left", "left", KEY_LEFT},
    [CTL_RIGHT] = {"Right", "right", KEY_RIGHT},
    [CTL_FAST_FORWARD] = {"Fast forward", "fastforward", 0},
    [CTL_SAVE_STATE] = {"Save state", "savestate", 0},
    [CTL_LOAD_STATE] = {"Load state", "loadstate", 0},
};

static const Binding defaults[CTL_COUNT] = {
    [CTL_A] = {SDL_SCANCODE_X, SDL_GAMEPAD_BUTTON_EAST},
    [CTL_B] = {SDL_SCANCODE_Z, SDL_GAMEPAD_BUTTON_SOUTH},
    [CTL_X] = {SDL_SCANCODE_S, SDL_GAMEPAD_BUTTON_NORTH},
    [CTL_Y] = {SDL_SCANCODE_A, SDL_GAMEPAD_BUTTON_WEST},
    [CTL_L] = {SDL_SCANCODE_Q, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    [CTL_R] = {SDL_SCANCODE_W, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    [CTL_START] = {SDL_SCANCODE_RETURN, SDL_GAMEPAD_BUTTON_START},
    [CTL_SELECT] = {SDL_SCANCODE_BACKSPACE, SDL_GAMEPAD_BUTTON_BACK},
    [CTL_UP] = {SDL_SCANCODE_UP, SDL_GAMEPAD_BUTTON_DPAD_UP},
    [CTL_DOWN] = {SDL_SCANCODE_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    [CTL_LEFT] = {SDL_SCANCODE_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    [CTL_RIGHT] = {SDL_SCANCODE_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    [CTL_FAST_FORWARD] = {SDL_SCANCODE_LSHIFT, PAD_RT},
    [CTL_SAVE_STATE] = {SDL_SCANCODE_UNKNOWN, PAD_NONE},
    [CTL_LOAD_STATE] = {SDL_SCANCODE_UNKNOWN, PAD_NONE},
};

static Binding binds[CTL_COUNT];
static SDL_GamepadType pad_type; /* of the last gamepad read, for its face button labels */
static int capture_action = -1, capture_pad, capture_conflict = -1;
static Uint64 capture_until;
static char ini_path[1024];

/* ---- controls.ini ---- */

static const char *pad_ini_name(int pad) {
    if (pad == PAD_LT) return SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    if (pad == PAD_RT) return SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    const char *s = pad >= 0 ? SDL_GetGamepadStringForButton((SDL_GamepadButton)pad) : NULL;
    return s ? s : "none";
}

static void save(void) {
    FILE *f = fopen(ini_path, "w");
    if (!f) return;
    fprintf(f, "# Egg of Time controls (System Settings > Controls): SDL key and gamepad button names, or none\n");
    for (int i = 0; i < CTL_COUNT; i++) {
        const char *k = binds[i].key != SDL_SCANCODE_UNKNOWN ? SDL_GetScancodeName(binds[i].key) : "";
        fprintf(f, "key.%s = %s\n", actions[i].ini, *k ? k : "none");
    }
    for (int i = 0; i < CTL_COUNT; i++) fprintf(f, "pad.%s = %s\n", actions[i].ini, pad_ini_name(binds[i].pad));
    fclose(f);
}

static void load(void) {
    FILE *f = fopen(ini_path, "r");
    if (!f) return;
    char line[160], dev[4], act[16], val[64];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " %3[a-z].%15[a-z] = %63[^\r\n]", dev, act, val) != 3) continue;
        for (size_t n = strlen(val); n && val[n - 1] == ' '; n--) val[n - 1] = 0;
        for (int i = 0; i < CTL_COUNT; i++) {
            if (strcmp(act, actions[i].ini)) continue;
            int none = !strcmp(val, "none");
            if (!strcmp(dev, "key")) {
                SDL_Scancode sc = SDL_GetScancodeFromName(val);
                if (none || sc != SDL_SCANCODE_UNKNOWN) binds[i].key = sc;
            } else if (!strcmp(dev, "pad")) {
                SDL_GamepadButton b = SDL_GetGamepadButtonFromString(val);
                SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(val);
                if (none) binds[i].pad = PAD_NONE;
                else if (b != SDL_GAMEPAD_BUTTON_INVALID) binds[i].pad = b;
                else if (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) binds[i].pad = PAD_LT;
                else if (a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) binds[i].pad = PAD_RT;
            }
        }
    }
    fclose(f);
}

void controls_init(void) {
    snprintf(ini_path, sizeof ini_path, "%scontrols.ini", rt_data_dir());
    memcpy(binds, defaults, sizeof binds);
    load();
}

void controls_reset(void) {
    memcpy(binds, defaults, sizeof binds);
    save();
}

/* ---- reading ---- */

static int pad_held(SDL_Gamepad *pad, int b) {
    if (!pad || b == PAD_NONE) return 0;
    if (b == PAD_LT) return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > AXIS_PRESSED;
    if (b == PAD_RT) return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > AXIS_PRESSED;
    return SDL_GetGamepadButton(pad, (SDL_GamepadButton)b);
}

int controls_held(SDL_Gamepad *pad, int action) {
    SDL_Scancode k = binds[action].key;
    return (k != SDL_SCANCODE_UNKNOWN && SDL_GetKeyboardState(NULL)[k]) || pad_held(pad, binds[action].pad);
}

uint32_t controls_read(SDL_Gamepad *pad) {
    uint32_t keys = 0;
    for (int i = 0; i < CTL_COUNT; i++) {
        if (actions[i].ds && controls_held(pad, i)) keys |= actions[i].ds;
    }
    if (pad) {
        pad_type = SDL_GetGamepadType(pad);
        int ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (ax < -AXIS_PRESSED) keys |= KEY_LEFT;
        if (ax > AXIS_PRESSED) keys |= KEY_RIGHT;
        if (ay < -AXIS_PRESSED) keys |= KEY_UP;
        if (ay > AXIS_PRESSED) keys |= KEY_DOWN;
    }
    if ((keys & (KEY_LEFT | KEY_RIGHT)) == (KEY_LEFT | KEY_RIGHT)) keys &= ~(KEY_LEFT | KEY_RIGHT);
    if ((keys & (KEY_UP | KEY_DOWN)) == (KEY_UP | KEY_DOWN)) keys &= ~(KEY_UP | KEY_DOWN);
    return keys;
}

/* ---- labels ---- */

const char *controls_name(int action) { return actions[action].name; }

const char *controls_key_label(int action) {
    const char *s = binds[action].key != SDL_SCANCODE_UNKNOWN ? SDL_GetScancodeName(binds[action].key) : "";
    return *s ? s : "-";
}

const char *controls_pad_label(int action) {
    static const char *const names[SDL_GAMEPAD_BUTTON_COUNT] = {
        [SDL_GAMEPAD_BUTTON_SOUTH] = "South",           [SDL_GAMEPAD_BUTTON_EAST] = "East",
        [SDL_GAMEPAD_BUTTON_WEST] = "West",             [SDL_GAMEPAD_BUTTON_NORTH] = "North",
        [SDL_GAMEPAD_BUTTON_BACK] = "Back",             [SDL_GAMEPAD_BUTTON_GUIDE] = "Guide",
        [SDL_GAMEPAD_BUTTON_START] = "Start",           [SDL_GAMEPAD_BUTTON_LEFT_STICK] = "L stick",
        [SDL_GAMEPAD_BUTTON_RIGHT_STICK] = "R stick",   [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = "L shoulder",
        [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = "R shoulder", [SDL_GAMEPAD_BUTTON_DPAD_UP] = "D-pad up",
        [SDL_GAMEPAD_BUTTON_DPAD_DOWN] = "D-pad down",  [SDL_GAMEPAD_BUTTON_DPAD_LEFT] = "D-pad left",
        [SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = "D-pad right", [SDL_GAMEPAD_BUTTON_MISC1] = "Misc",
        [SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1] = "R paddle 1", [SDL_GAMEPAD_BUTTON_LEFT_PADDLE1] = "L paddle 1",
        [SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2] = "R paddle 2", [SDL_GAMEPAD_BUTTON_LEFT_PADDLE2] = "L paddle 2",
        [SDL_GAMEPAD_BUTTON_TOUCHPAD] = "Touchpad",
    };
    static const char *const face[] = {
        [SDL_GAMEPAD_BUTTON_LABEL_A] = "A",           [SDL_GAMEPAD_BUTTON_LABEL_B] = "B",
        [SDL_GAMEPAD_BUTTON_LABEL_X] = "X",           [SDL_GAMEPAD_BUTTON_LABEL_Y] = "Y",
        [SDL_GAMEPAD_BUTTON_LABEL_CROSS] = "Cross",   [SDL_GAMEPAD_BUTTON_LABEL_CIRCLE] = "Circle",
        [SDL_GAMEPAD_BUTTON_LABEL_SQUARE] = "Square", [SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE] = "Triangle",
    };
    int b = binds[action].pad;
    if (b == PAD_NONE) return "-";
    if (b == PAD_LT) return "L trigger";
    if (b == PAD_RT) return "R trigger";
    /* Face buttons by the connected gamepad's own letters (East reads A on a Nintendo pad, B on an Xbox one). */
    SDL_GamepadButtonLabel l = SDL_GetGamepadButtonLabelForType(pad_type, (SDL_GamepadButton)b);
    if (l != SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN && l < (int)SDL_arraysize(face) && face[l]) return face[l];
    return names[b] ? names[b] : pad_ini_name(b);
}

/* ---- rebinding ---- */

void controls_capture_start(int action, int pad) {
    capture_action = action;
    capture_pad = pad;
    capture_conflict = -1;
    capture_until = SDL_GetTicks() + CAPTURE_MS;
}

void controls_capture_cancel(void) { capture_action = -1; }

int controls_capturing(void) {
    if (capture_action >= 0 && SDL_GetTicks() >= capture_until) capture_action = -1;
    return capture_action >= 0;
}

const char *controls_capture_conflict(void) { return capture_conflict >= 0 ? actions[capture_conflict].name : NULL; }

static int is_state_slot(int a) { return a == CTL_SAVE_STATE || a == CTL_LOAD_STATE; }

/* Gives action a the input, swapping as controls.h says; 0 if it is refused (capture_conflict: whose it is). */
static int bind_key(int a, SDL_Scancode k) {
    for (int i = 0; i < CTL_COUNT; i++) {
        if (i == a || binds[i].key != k) continue;
        if (!is_state_slot(i) && is_state_slot(a) && binds[a].key == SDL_SCANCODE_UNKNOWN) {
            capture_conflict = i;
            return 0;
        }
        binds[i].key = is_state_slot(i) ? SDL_SCANCODE_UNKNOWN : binds[a].key;
    }
    binds[a].key = k;
    return 1;
}

static int bind_pad(int a, int b) {
    for (int i = 0; i < CTL_COUNT; i++) {
        if (i == a || binds[i].pad != b) continue;
        if (!is_state_slot(i) && is_state_slot(a) && binds[a].pad == PAD_NONE) {
            capture_conflict = i;
            return 0;
        }
        binds[i].pad = is_state_slot(i) ? PAD_NONE : binds[a].pad;
    }
    binds[a].pad = b;
    return 1;
}

int controls_capture(const SDL_Event *e) {
    if (!controls_capturing()) return 0;
    int b = PAD_NONE;
    if (e->type == SDL_EVENT_KEY_DOWN) {
        SDL_Scancode k = e->key.scancode;
        if (k == SDL_SCANCODE_ESCAPE) capture_action = -1;
        else if (!capture_pad && !e->key.repeat && k != SDL_SCANCODE_UNKNOWN && !(k >= SDL_SCANCODE_F1 && k <= SDL_SCANCODE_F12)) {
            if (bind_key(capture_action, k)) {
                capture_action = -1;
                save();
            }
        }
        return 1;
    }
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) b = e->gbutton.button;
    else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e->gaxis.value > AXIS_PRESSED) {
        if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) b = PAD_LT;
        if (e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) b = PAD_RT;
    }
    if (b == PAD_NONE) return 0;
    if (capture_pad && bind_pad(capture_action, b)) {
        capture_action = -1;
        save();
    }
    return 1;
}
