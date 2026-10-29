/* Host entry point: SDL3 window, input and presentation; the game runs on its own thread. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "display.h"
#include "game_build.h"
#include "rt.h"
#include "sound.h"
#include "system_menu.h"
#include "video.h"

static SDL_AtomicInt game_finished;

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO, fmt, ap);
    va_end(ap);
}

_Noreturn void rt_fatal(CPU *cpu, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "fatal: %s", msg);
    if (cpu) {
        for (int i = 0; i < 16; i += 4) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x", i, cpu->r[i],
                         i + 1, cpu->r[i + 1], i + 2, cpu->r[i + 2], i + 3, cpu->r[i + 3]);
        }
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  call depth %u, innermost return 0x%08x", cpu->ret_depth,
                     cpu->ret_depth ? cpu->ret_stack[cpu->ret_depth - 1] : 0);
    }
    /* EGG_DUMP_RAM=<file>: also dump main RAM (<file>) and ITCM (<file>.itcm) at a fatal error. */
    const char *dump = getenv("EGG_DUMP_RAM");
    if (dump) {
        char path[1024];
        FILE *f = fopen(dump, "wb");
        if (f) { fwrite(rt_main_ram, 1, RT_MAIN_RAM_SIZE, f); fclose(f); }
        snprintf(path, sizeof path, "%s.itcm", dump);
        f = fopen(path, "wb");
        if (f) { fwrite(rt_itcm, 1, RT_ITCM_SIZE, f); fclose(f); }
    }
    fflush(stderr);
    exit(1);
}

static int SDLCALL game_thread(void *arg) {
    (void)arg;
    rt_threads_run(rt_boot);
    SDL_SetAtomicInt(&game_finished, 1);
    return 0;
}

/* ---- input ---- */

typedef struct KeyMap { SDL_Scancode sc; uint32_t key; } KeyMap;
static const KeyMap keymap[] = {
    {SDL_SCANCODE_X, KEY_A},          {SDL_SCANCODE_Z, KEY_B},          {SDL_SCANCODE_S, KEY_X},
    {SDL_SCANCODE_A, KEY_Y},          {SDL_SCANCODE_Q, KEY_L},          {SDL_SCANCODE_W, KEY_R},
    {SDL_SCANCODE_RETURN, KEY_START}, {SDL_SCANCODE_RSHIFT, KEY_SELECT}, {SDL_SCANCODE_BACKSPACE, KEY_SELECT},
    {SDL_SCANCODE_UP, KEY_UP},        {SDL_SCANCODE_DOWN, KEY_DOWN},     {SDL_SCANCODE_LEFT, KEY_LEFT},
    {SDL_SCANCODE_RIGHT, KEY_RIGHT},
};

/* Positional mapping: the DS A button is the right face button, as on Nintendo controllers. */
typedef struct PadMap { SDL_GamepadButton b; uint32_t key; } PadMap;
static const PadMap padmap[] = {
    {SDL_GAMEPAD_BUTTON_EAST, KEY_A},          {SDL_GAMEPAD_BUTTON_SOUTH, KEY_B},
    {SDL_GAMEPAD_BUTTON_NORTH, KEY_X},         {SDL_GAMEPAD_BUTTON_WEST, KEY_Y},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, KEY_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, KEY_R},
    {SDL_GAMEPAD_BUTTON_START, KEY_START},     {SDL_GAMEPAD_BUTTON_BACK, KEY_SELECT},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, KEY_UP},      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, KEY_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, KEY_LEFT},  {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, KEY_RIGHT},
};

/* EGG_AUTOKEYS="<ms>:<KEY>:<ms>,...": press keys at fixed times after start (automated tests).
 * KEY TOUCH taps the bottom screen at EGG_TOUCH_POS="x,y" (default: the centre); SAVESTATE / LOADSTATE
 * act like F5 / F7, LAYOUT / SWAP / FILTER like F2 / F3 / F4, ESC like holding Escape. */
#define AUTO_TOUCH (1u << 31)
#define AUTO_SAVE (1u << 30)
#define AUTO_LOAD (1u << 29)
#define AUTO_LAYOUT (1u << 28)
#define AUTO_SWAP (1u << 27)
#define AUTO_FILTER (1u << 26)
#define AUTO_ESC (1u << 25)
#define AUTO_HOST (AUTO_TOUCH | AUTO_SAVE | AUTO_LOAD | AUTO_LAYOUT | AUTO_SWAP | AUTO_FILTER | AUTO_ESC)
typedef struct AutoKey { Uint64 at, until; uint32_t key; } AutoKey;
static AutoKey autokeys[64];
static int autokey_count;

static void parse_autokeys(const char *spec) {
    static const struct { const char *name; uint32_t key; } names[] = {
        {"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y}, {"L", KEY_L}, {"R", KEY_R}, {"START", KEY_START},
        {"SELECT", KEY_SELECT}, {"UP", KEY_UP}, {"DOWN", KEY_DOWN}, {"LEFT", KEY_LEFT}, {"RIGHT", KEY_RIGHT},
        {"TOUCH", AUTO_TOUCH}, {"SAVESTATE", AUTO_SAVE}, {"LOADSTATE", AUTO_LOAD},
        {"LAYOUT", AUTO_LAYOUT}, {"SWAP", AUTO_SWAP}, {"FILTER", AUTO_FILTER}, {"ESC", AUTO_ESC},
    };
    while (spec && *spec && autokey_count < (int)SDL_arraysize(autokeys)) {
        char name[16];
        unsigned long at, dur;
        int used = 0;
        if (sscanf(spec, "%lu:%15[A-Z]:%lu%n", &at, name, &dur, &used) != 3) break;
        for (size_t i = 0; i < SDL_arraysize(names); i++) {
            if (!strcmp(names[i].name, name)) autokeys[autokey_count++] = (AutoKey){at, at + dur, names[i].key};
        }
        spec += used;
        if (*spec == ',') spec++;
    }
}

static uint32_t auto_keys(Uint64 now) {
    uint32_t keys = 0;
    for (int i = 0; i < autokey_count; i++) {
        if (now >= autokeys[i].at && now < autokeys[i].until) keys |= autokeys[i].key;
    }
    return keys;
}

static uint32_t read_keys(SDL_Gamepad *pad) {
    const bool *ks = SDL_GetKeyboardState(NULL);
    uint32_t keys = 0;
    for (size_t i = 0; i < SDL_arraysize(keymap); i++) {
        if (ks[keymap[i].sc]) keys |= keymap[i].key;
    }
    if (pad) {
        for (size_t i = 0; i < SDL_arraysize(padmap); i++) {
            if (SDL_GetGamepadButton(pad, padmap[i].b)) keys |= padmap[i].key;
        }
        int ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (ax < -16000) keys |= KEY_LEFT;
        if (ax > 16000) keys |= KEY_RIGHT;
        if (ay < -16000) keys |= KEY_UP;
        if (ay > 16000) keys |= KEY_DOWN;
    }
    if ((keys & (KEY_LEFT | KEY_RIGHT)) == (KEY_LEFT | KEY_RIGHT)) keys &= ~(KEY_LEFT | KEY_RIGHT);
    if ((keys & (KEY_UP | KEY_DOWN)) == (KEY_UP | KEY_DOWN)) keys &= ~(KEY_UP | KEY_DOWN);
    return keys;
}

/* ---- presentation ---- */

static uint32_t top_px[SCREEN_W * SCREEN_H], bottom_px[SCREEN_W * SCREEN_H], main_px[VIDEO_LINE_MAX * SCREEN_H];
static const uint32_t *const screens[3] = {top_px, bottom_px, main_px};
static int main_w = SCREEN_W; /* main_px: rows of this width (widescreen) */
static uint32_t party_px[VIDEO_LINE_MAX * SCREEN_H]; /* main screen's party layer (camera smoothing) */
static int party_on;


/* EGG_SHOTS=<dir>: save the screens (top, bottom, and the main screen of single screen mode, which may be
 * wider, stacked, native, black beside narrower ones) and the presented window as BMP files, every
 * EGG_SHOT_MS ms (default 3000) starting EGG_SHOT_FROM ms after start (bring-up aid; EGG_SHOT_MS=1 captures
 * every presented frame). */
static void save_shot(const char *dir, int n) {
    SDL_Surface *s = SDL_CreateSurface(main_w, SCREEN_H * 3, SDL_PIXELFORMAT_ABGR8888);
    if (!s) return;
    SDL_ClearSurface(s, 0, 0, 0, 1);
    for (int i = 0; i < 3; i++) {
        int w = i == 2 ? main_w : SCREEN_W;
        for (int y = 0; y < SCREEN_H; y++) {
            memcpy((uint8_t *)s->pixels + (size_t)(y + i * SCREEN_H) * s->pitch, &screens[i][y * w], (size_t)w * 4);
        }
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/shot_%04d.bmp", dir, n);
    if (SDL_SaveBMP(s, path)) rt_log("screenshot %s", path);
    SDL_DestroySurface(s);
    snprintf(path, sizeof path, "%s/window_%04d.bmp", dir, n);
    display_capture(path);
}

int main(int argc, char **argv) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!display_init()) return 1;
    if (!game_build_prepare(argc > 1 ? argv[1] : NULL)) return 1; /* the ROM and the game code */
    spu_open_output();

    /* Deep C call chains (recompiled calls + tail calls) need a large host stack. */
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)game_thread);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "arm9");
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 64 << 20);
    SDL_Thread *t = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    if (!t) {
        fprintf(stderr, "SDL_CreateThread: %s\n", SDL_GetError());
        return 1;
    }
    SDL_DetachThread(t);

    /* EGG_DUMP_RAM=<file>: write main RAM to <file>.<n> at the times (ms) listed in EGG_DUMP_AT
     * (default 3000) (bring-up aid). */
    const char *dump_path = getenv("EGG_DUMP_RAM");
    const char *dump_times = getenv("EGG_DUMP_AT") ? getenv("EGG_DUMP_AT") : "3000";
    Uint64 start_ticks = SDL_GetTicks();
    int dump_n = 0;
    /* EGG_WATCH=1: every 2 s, log the running game thread's call stack (racy snapshot; bring-up aid). */
    int watch = getenv("EGG_WATCH") != NULL;
    Uint64 watch_at = SDL_GetTicks() + 2000;
    const char *shots = getenv("EGG_SHOTS");
    Uint64 shot_every = getenv("EGG_SHOT_MS") ? (Uint64)atoi(getenv("EGG_SHOT_MS")) : 3000;
    Uint64 shot_at = SDL_GetTicks() + (getenv("EGG_SHOT_FROM") ? (Uint64)atoi(getenv("EGG_SHOT_FROM")) : shot_every);
    int shot_n = 0;

    parse_autokeys(getenv("EGG_AUTOKEYS"));
    /* EGG_LOAD_STATE=<file>: start from a save state. */
    if (getenv("EGG_LOAD_STATE")) {
        snprintf(rt_state_load_path, sizeof rt_state_load_path, "%s", getenv("EGG_LOAD_STATE"));
        rt_state_request = RT_STATE_LOAD;
    }
    uint32_t auto_prev = 0;
    SDL_Gamepad *pad = NULL;
    uint64_t shown_serial = 0, shown_t = 0, game_period = SDL_NS_PER_SECOND / 60;
    int running = 1;
    uint64_t next_present = SDL_GetTicksNS();
    /* EGG_LOG_FPS=1: log window frames and game frames (VBlanks) per second. */
    int log_fps = getenv("EGG_LOG_FPS") != NULL, presented = 0;
    Uint64 fps_at = SDL_GetTicks() + 1000, game_frames = rt_frame_count;
    /* Holding Escape for EXIT_HOLD_MS quits; the message shows while it is held. */
    enum { EXIT_HOLD_MS = 1000 };
    Uint64 esc_since = 0;
    while (running && !SDL_GetAtomicInt(&game_finished)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && display_key(e.key.scancode)) {
                continue;
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F5 && !e.key.repeat) {
                rt_state_request = RT_STATE_SAVE;
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F7 && !e.key.repeat) {
                rt_state_load_path[0] = 0;
                rt_state_request = RT_STATE_LOAD;
            } else if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) {
                pad = SDL_OpenGamepad(e.gdevice.which);
            } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) {
                SDL_CloseGamepad(pad);
                pad = NULL;
            }
        }
        uint32_t autok = auto_keys(SDL_GetTicks());
        if (autok & ~auto_prev & AUTO_SAVE) rt_state_request = RT_STATE_SAVE;
        if (autok & ~auto_prev & AUTO_LOAD) {
            rt_state_load_path[0] = 0;
            rt_state_request = RT_STATE_LOAD;
        }
        if (autok & ~auto_prev & AUTO_LAYOUT) display_key(SDL_SCANCODE_F2);
        if (autok & ~auto_prev & AUTO_SWAP) display_key(SDL_SCANCODE_F3);
        if (autok & ~auto_prev & AUTO_FILTER) display_key(SDL_SCANCODE_F4);
        auto_prev = autok;
        if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_ESCAPE] || (autok & AUTO_ESC)) {
            if (!esc_since) esc_since = SDL_GetTicks();
            display_message("Exiting...", 100);
            if (SDL_GetTicks() - esc_since >= EXIT_HOLD_MS) running = 0;
        } else {
            esc_since = 0;
        }
        rt_keys = sysmenu_input(read_keys(pad) | (autok & ~AUTO_HOST));

        /* Touch: left mouse button on the bottom screen, when it is shown. */
        float mx, my;
        SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
        int tx_, ty_;
        int touching = (mb & SDL_BUTTON_LMASK) && display_touch(mx, my, &tx_, &ty_);
        if (touching) {
            rt_touch_x = tx_;
            rt_touch_y = ty_;
        }
        if (auto_keys(SDL_GetTicks()) & AUTO_TOUCH) {
            int tx = SCREEN_W / 2, ty = SCREEN_H / 2;
            /* EGG_TOUCH_POS="x,y[;x,y...]": the n-th TOUCH entry of EGG_AUTOKEYS uses the n-th position. */
            const char *pos = getenv("EGG_TOUCH_POS");
            Uint64 now = SDL_GetTicks();
            int idx = 0;
            for (int i = 0; i < autokey_count; i++) {
                if (autokeys[i].key != AUTO_TOUCH) continue;
                if (now >= autokeys[i].at && now < autokeys[i].until) break;
                idx++;
            }
            for (int i = 0; pos && *pos; i++) {
                int x, y;
                if (sscanf(pos, "%d,%d", &x, &y) == 2) { tx = x; ty = y; }
                if (i == idx) break;
                pos = strchr(pos, ';');
                if (pos) pos++;
            }
            touching = 1;
            rt_touch_x = tx;
            rt_touch_y = ty;
        }
        rt_touch_down = touching;

        int cam_dx, cam_dy;
        uint64_t frame_t, serial = video_frame_serial(&cam_dx, &cam_dy, &frame_t);
        int new_frame = serial != shown_serial;
        if (new_frame) {
            if (shown_serial && frame_t > shown_t) { /* measured game frame period */
                game_period = (game_period * 7 + SDL_clamp(frame_t - shown_t, 8000000u, 34000000u)) / 8;
            }
            shown_serial = serial;
            shown_t = frame_t;
            main_w = video_copy_frame(top_px, bottom_px, main_px, party_px, &party_on);
        }
        /* Camera smoothing: show the view slid back by the share of the camera's last step still to come, timed from
         * the frame's arrival (so every window frame lands at its own position, whatever the refresh rate). */
        float behind = display_camera_smoothing() ? 1.0f - SDL_min(1.0f, (float)(SDL_GetTicksNS() - shown_t) / game_period) : 0;
        if (sysmenu_is_open()) {
            main_w = SCREEN_W;
            party_on = 0;
            sysmenu_draw(main_px);
            memcpy(top_px, main_px, sizeof top_px); /* layouts other than top-only show it too */
            new_frame = 1;
        }
        display_frame(screens, party_on ? party_px : NULL, main_w, new_frame, -cam_dx * behind, -cam_dy * behind);
        presented++;
        if (log_fps && SDL_GetTicks() >= fps_at) {
            rt_log("fps: window %d, game %llu", presented, (unsigned long long)(rt_frame_count - game_frames));
            presented = 0;
            game_frames = rt_frame_count;
            fps_at += 1000;
        }
        /* Framerate: pace the window's frames (the game runs on its own clock). */
        uint64_t frame_ns = display_frame_ns();
        if (frame_ns) {
            uint64_t t = SDL_GetTicksNS();
            next_present = next_present + frame_ns > t ? next_present + frame_ns : t; /* never catch up a backlog */
            if (next_present > t) SDL_DelayPrecise(next_present - t);
        }

        Uint64 now = SDL_GetTicks();
        if (shots && now >= shot_at) {
            shot_at = SDL_max(shot_at + shot_every, now);
            save_shot(shots, ++shot_n);
        }
        if (dump_path && *dump_times && now >= start_ticks + (Uint64)atoi(dump_times)) {
            char path[1024];
            snprintf(path, sizeof path, "%s.%d", dump_path, ++dump_n);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(rt_main_ram, 1, RT_MAIN_RAM_SIZE, f);
                fclose(f);
                rt_log("main RAM dumped to %s", path);
            }            const char *comma = strchr(dump_times, ',');
            dump_times = comma ? comma + 1 : "";
        }
        if (watch && now >= watch_at) {
            watch_at += 2000;
            CPU *c = rt_cur_cpu;
            if (c) {
                char buf[256];
                int n = 0;
                for (uint32_t i = c->ret_depth; i > 0 && c->ret_depth - i < 12 && n < (int)sizeof buf - 12; i--) {
                    n += snprintf(buf + n, sizeof buf - (size_t)n, " %08x", c->ret_stack[i - 1]);
                }
                rt_log("watch: frame %llu, depth %u, returns:%s", (unsigned long long)rt_frame_count, c->ret_depth, buf);
                rt_threads_log();
                rt_log("watch: IME=%x IE=%08x IF=%08x DISPSTAT=%04x irqcheck=%08x irqqueue=%08x", io_get32(0x208),
                       io_get32(0x210), irq_if(), io_get16(0x004), rt_r32(rt_dtcm_base + 0x3FF8u), rt_r32(0x027e0060u));
                rt_log("watch: DISPCNT A=%08x B=%08x POWCNT1=%04x VRAMCNT=%08x %08x %04x MASTER_BRIGHT A=%04x B=%04x",
                       io_get32(0x000), io_get32(0x1000), io_get16(0x304), io_get32(0x240), io_get32(0x244),
                       io_get16(0x248), io_get16(0x06C), io_get16(0x106C));
            }
        }
    }
    /* SDL_Quit frees what the game thread uses (the audio stream, ...): stop it first. If it does not stop
     * (code that never polls), the end of the process cleans up instead. */
    if (SDL_GetAtomicInt(&game_finished) || rt_stop_game(1000)) SDL_Quit();
    return 0;
}
