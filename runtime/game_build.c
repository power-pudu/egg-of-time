/* The game library and the ROM, ready before the game starts.
 *
 * The recompiled game code is a library of its own (game/eggoftime_game.dll in the data folder), because it is a
 * translation of the game's code and must not be distributed: a release holds the runtime and the tools to make it
 * (kit/), and builds it on the player's machine from their ROM on the first start:
 *   1. ROM: rom/CT.nds in the data folder, or one the player picks (copied there, so saves stay beside it);
 *      its SHA-1 must be one the recompiler has a config for (USA, Europe, Japan).
 *   2. kit/dsd/dsd.exe rom extract                -> game/build/extract (ARM9 binaries, overlays)
 *   3. kit/python/python.exe kit/recomp/recomp.py  -> game/build/gen (C sources, for the ROM's version)
 *   4. kit/llvm/bin/clang.exe, one process per core -> game/build/obj, then linked against kit/lib/libeggoftime.a
 *      (the executable's import library) into game/eggoftime_game.dll.
 *   5. game/build.id records what it was built from (kit/kit.id and the ROM's SHA-1); game/build/ is deleted.
 * The Linux release bundles dsd (kit/dsd/dsd) and Zig (kit/zig/zig, run as "zig cc"), a compiler with its own
 * libc headers: SteamOS has no compiler and no /usr/include. Python is the system's (python3, found in PATH); without
 * kit/zig (an older kit, or one trimmed by hand) so is the C compiler (cc, clang or gcc). The library links against
 * nothing, its runtime symbols resolved from the executable (-rdynamic) when it is loaded.
 * A later start loads the library when game/build.id still matches, else builds it again (a new release whose
 * recompiler or runtime interface changed). The tools' output goes to game/build.log.
 * Developer builds have no kit/: CMake builds the library, and this only loads it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "dispatch.h"
#include "display.h"
#include "game_build.h"
#include "rt.h"
#include "translation.h"
#include "ui_font.h"
#include "video.h"

#define DEFAULT_ROM "rom/CT.nds"
/* The versions of the game the recompiler has a config for (config/, config/regions): their unmodified dumps. */
static const char *const ROM_SHA1[] = {
    "11f9e8204e484eab42181b210c9f214584be75bf", /* USA, YQUE */
    "a6c69d789a6f2e31b27fba950423bea3c2f30a37", /* Europe, YQUP */
    "afc91a3f1f6c1bd02845b76416239e4dabfae19f", /* Japan, YQUJ */
};
#ifdef _WIN32
#define GAME_LIBRARY "game/eggoftime_game.dll"
#define EXE ".exe"
#else
#define GAME_LIBRARY "game/eggoftime_game.so"
#define EXE ""
#endif

static char rom_sha[41];   /* the loaded ROM's SHA-1 (releases) */

static void join(char *out, size_t n, const char *dir, const char *rel) {
    if ((size_t)snprintf(out, n, "%s%s", dir, rel) >= n) out[0] = 0; /* too long: no such file */
}

/* kit/ is part of the install; rom/ and game/ are written, so they are in the data folder (rt.h). */
static void kit_path(char *out, size_t n, const char *rel) { join(out, n, rt_install_dir(), rel); }
static void data_path(char *out, size_t n, const char *rel) { join(out, n, rt_data_dir(), rel); }

/* ---- SHA-1 ---- */

static uint32_t rol(uint32_t v, int s) { return v << s | v >> (32 - s); }

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
        else f = b ^ c ^ d, k = 0xCA62C1D6u;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

static void sha1_hex(const uint8_t *data, size_t n, char out[41]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    size_t full = n & ~(size_t)63;
    for (size_t i = 0; i < full; i += 64) sha1_block(h, data + i);
    uint8_t tail[128] = {0};
    size_t rest = n - full, len = rest < 56 ? 64 : 128;
    memcpy(tail, data + full, rest);
    tail[rest] = 0x80;
    for (int i = 0; i < 8; i++) tail[len - 1 - i] = (uint8_t)((uint64_t)n * 8 >> (8 * i));
    for (size_t i = 0; i < len; i += 64) sha1_block(h, tail + i);
    for (int i = 0; i < 5; i++) snprintf(out + 8 * i, 9, "%08x", h[i]);
}

/* ---- screens ---- */

static uint32_t screen[SCREEN_W * SCREEN_H], blank[SCREEN_W * SCREEN_H];

/* Shows lines of text centred on the screen, and a progress bar when progress >= 0 (0..1). */
static void show(const char *const *lines, int count, float progress) {
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) screen[i] = blank[i] = 0xFF000000u;
    int y = (SCREEN_H - count * (UI_FONT_H + 6)) / 2 - (progress >= 0 ? 8 : 0);
    for (int i = 0; i < count; i++, y += UI_FONT_H + 6) {
        uint32_t color = i == 0 ? 0xFF40D0FFu : 0xFFFFFFFFu; /* ABGR: the first line amber */
        ui_draw_text(screen, lines[i], (SCREEN_W - ui_text_width(lines[i])) / 2, y, color);
    }
    if (progress >= 0) {
        int x0 = 32, w = SCREEN_W - 64, fill = (int)(w * SDL_clamp(progress, 0.0f, 1.0f));
        for (int yy = y + 8; yy < y + 14; yy++) {
            for (int x = 0; x < w; x++) screen[yy * SCREEN_W + x0 + x] = x < fill ? 0xFF40D0FFu : 0xFF404040u;
        }
    }
    const uint32_t *const screens[3] = {screen, blank, screen};
    display_frame(screens, NULL, NULL, NULL, SCREEN_W, SCREEN_H, 1, 0, 0, NULL);
}

/* Handles window events; 0 when the player closes the window. *key is set on a key press or click. */
static int pump(int *key) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        display_event(&e);
        if (e.type == SDL_EVENT_QUIT) return 0;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (display_key(e.key.scancode)) continue;
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) return 0;
            if (key) *key = 1;
        }
        if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) && key) *key = 1;
    }
    return 1;
}

/* Shows the lines until a key press (1) or the window closes (0). */
static int wait_key(const char *const *lines, int count) {
    for (;;) {
        int key = 0;
        if (!pump(&key)) return 0;
        if (key) return 1;
        show(lines, count, -1);
        SDL_Delay(16);
    }
}

/* ---- the ROM ---- */

static SDL_AtomicInt picked; /* 1: a file was chosen (picked_path), -1: cancelled */
static char picked_path[1024];

static void SDLCALL on_pick(void *user, const char *const *files, int filter) {
    (void)user, (void)filter;
    if (files && files[0]) snprintf(picked_path, sizeof picked_path, "%s", files[0]);
    SDL_SetAtomicInt(&picked, files && files[0] ? 1 : -1);
}

/* Asks for the ROM file and loads it; 0 if the window was closed. */
static int pick_rom(void) {
    static const SDL_DialogFileFilter filter = {"Nintendo DS ROM (*.nds)", "nds"};
    static const char *const ask[] = {"Egg of Time", "Choose your Chrono Trigger ROM", "(a .nds file)"};
    for (;;) {
        SDL_SetAtomicInt(&picked, 0);
        SDL_ShowOpenFileDialog(on_pick, NULL, NULL, &filter, 1, NULL, false);
        while (!SDL_GetAtomicInt(&picked)) {
            if (!pump(NULL)) return 0;
            show(ask, 3, -1);
            SDL_Delay(16);
        }
        if (SDL_GetAtomicInt(&picked) > 0 && rt_load_rom(picked_path)) return 1;
        static const char *const none[] = {"No ROM chosen", "The game needs your own Chrono Trigger ROM.",
                                           "Press a key to choose it, or Esc to quit."};
        if (!wait_key(none, 3)) return 0;
    }
}

/* Loads the ROM (and in a release makes sure it is the right one, kept as rom/CT.nds); 0 if the player quit. */
static int load_rom(const char *rom_arg, int release) {
    char rom[1024];
    data_path(rom, sizeof rom, DEFAULT_ROM);
    if (rom_arg) snprintf(rom, sizeof rom, "%s", rom_arg);
    int ok = rt_load_rom(rom) || (!rom_arg && rt_load_rom(DEFAULT_ROM));
    if (!release) {
        if (!ok) {
            char msg[1200];
            snprintf(msg, sizeof msg, "Cannot load the ROM '%s'.\nUsage: \"Egg of Time\" [path/to/rom.nds]", rom);
            fprintf(stderr, "%s\n", msg);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Egg of Time", msg, NULL);
        }
        return ok;
    }
    for (;;) {
        if (!ok && !pick_rom()) return 0;
        sha1_hex(rt_rom.data, rt_rom.size, rom_sha);
        int known = 0;
        for (size_t i = 0; i < SDL_arraysize(ROM_SHA1); i++) known |= !strcmp(rom_sha, ROM_SHA1[i]);
        if (known) break;
        rt_log("rom: SHA-1 %s is not a known dump (game code %s)", rom_sha, rt_rom.code);
        static const char *const wrong[] = {"This is not the right ROM", "Egg of Time needs Chrono Trigger for the DS",
                                            "(USA, Europe or Japan), an unmodified dump.",
                                            "Press a key to choose another file, or Esc to quit."};
        if (!wait_key(wrong, 4)) return 0;
        ok = 0;
    }
    /* Keep a copy in rom/ in the data folder, so it is found next time and the saves stay beside it. */
    char kept[1024], dir[1024];
    data_path(kept, sizeof kept, DEFAULT_ROM);
    if (strcmp(rt_rom.path, kept) != 0) {
        data_path(dir, sizeof dir, "rom");
        SDL_CreateDirectory(dir);
        if (SDL_SaveFile(kept, rt_rom.data, rt_rom.size)) {
            free(rt_rom.data);
            if (!rt_load_rom(kept)) return 0;
        } else {
            rt_log("rom: cannot copy to %s: %s", kept, SDL_GetError());
        }
    }
    return 1;
}

/* ---- building ---- */

enum { MAX_JOBS = 64 };

static struct {
    SDL_Mutex *lock;
    SDL_IOStream *log;
    SDL_Process *running[MAX_JOBS]; /* to stop them when the player quits */
    const char *step;               /* shown on screen (under lock) */
    char **sources;                 /* game/build/gen/<name>.c */
    int source_count;
    SDL_AtomicInt next, compiled, failed, cancel, done;
    char rom[1024];
    char python[1024], cc[1024]; /* the tools that translate and compile the game's code */
    int zig;                      /* cc is the kit's Zig: "zig cc", for glibc on x86-64 */
} b;

/* The compiler's command line up to its own arguments, in args; returns how many. */
static int cc_command(const char **args) {
    int k = 0;
    args[k++] = b.cc;
    if (b.zig) { /* -g0: zig adds debug info otherwise */
        args[k++] = "cc", args[k++] = "-target", args[k++] = "x86_64-linux-gnu", args[k++] = "-g0";
    }
    return k;
}

/* Finds the tools: the kit's on Windows, the system's on Linux; 0 (and tells the player) if one is missing. */
static int find_tools(void) {
#ifdef _WIN32
    kit_path(b.python, sizeof b.python, "kit/python/python.exe");
    kit_path(b.cc, sizeof b.cc, "kit/llvm/bin/clang.exe");
    return 1;
#else
    kit_path(b.cc, sizeof b.cc, "kit/zig/zig");
    b.zig = access(b.cc, X_OK) == 0;
    if (b.zig) {
        /* Its caches in the build folder, deleted with it (the kit can be read-only: the AppImage). */
        char cache[1024];
        data_path(cache, sizeof cache, "game/build/zig-cache");
        SDL_Environment *env = SDL_GetEnvironment();
        SDL_SetEnvironmentVariable(env, "ZIG_GLOBAL_CACHE_DIR", cache, true);
        SDL_SetEnvironmentVariable(env, "ZIG_LOCAL_CACHE_DIR", cache, true);
    }
    static const char *const pythons[] = {"python3", NULL}, *const compilers[] = {"cc", "clang", "gcc", NULL};
    const char *const *want[2] = {pythons, compilers};
    char *found[2] = {b.python, b.cc};
    const char *dirs = getenv("PATH");
    for (int t = 0; t < 2 - b.zig; t++) {
        found[t][0] = 0;
        for (int i = 0; want[t][i] && !found[t][0]; i++) {
            for (const char *d = dirs ? dirs : "/usr/bin"; *d && !found[t][0]; d += strcspn(d, ":"), d += *d == ':') {
                int n = (int)strcspn(d, ":");
                snprintf(found[t], 1024, "%.*s/%s", n, d, want[t][i]);
                if (!n || access(found[t], X_OK)) found[t][0] = 0;
            }
        }
    }
    if (b.python[0] && b.cc[0]) return 1;
    static const char *const missing[] = {"Python 3 and a C compiler are needed",
                                          "The first start builds the game with them.",
                                          "Debian/Ubuntu: sudo apt install python3 build-essential",
                                          "Fedora: sudo dnf install python3 gcc", "Arch: sudo pacman -S python gcc",
                                          "Press a key to quit."};
    static const char *const no_python[] = {"Python 3 is needed", "The first start builds the game with it.",
                                            "Debian/Ubuntu: sudo apt install python3", "Fedora: sudo dnf install python3",
                                            "Arch: sudo pacman -S python", "Press a key to quit."};
    rt_log("build: %s not found in PATH", b.python[0] ? "cc, clang or gcc" : "python3");
    wait_key(b.zig ? no_python : missing, 6);
    return 0;
#endif
}

static void log_text(const char *s, size_t n) {
    SDL_LockMutex(b.lock);
    if (b.log) SDL_WriteIO(b.log, s, n);
    SDL_UnlockMutex(b.lock);
}

/* Runs a tool to its end, its output going to the log; its exit code (-1 if it could not start). */
static int run(const char *const *args, int slot) {
    char line[2048];
    int n = snprintf(line, sizeof line, ">");
    for (int i = 0; args[i] && n < (int)sizeof line - 1; i++) n += snprintf(line + n, sizeof line - n, " %s", args[i]);
    snprintf(line + SDL_min(n, (int)sizeof line - 2), 2, "\n");
    log_text(line, strlen(line));
    if (SDL_GetAtomicInt(&b.cancel)) return -1;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!p) {
        n = snprintf(line, sizeof line, "cannot start %s: %s\n", args[0], SDL_GetError());
        log_text(line, (size_t)n);
        return -1;
    }
    SDL_LockMutex(b.lock);
    b.running[slot] = p;
    SDL_UnlockMutex(b.lock);
    size_t size = 0;
    int code = -1;
    void *out = SDL_ReadProcess(p, &size, &code);
    if (out) log_text(out, size), SDL_free(out);
    SDL_LockMutex(b.lock);
    b.running[slot] = NULL;
    SDL_UnlockMutex(b.lock);
    SDL_DestroyProcess(p);
    if (code) {
        n = snprintf(line, sizeof line, "exit code %d\n", code);
        log_text(line, (size_t)n);
    }
    return code;
}

static void set_step(const char *s) {
    SDL_LockMutex(b.lock);
    b.step = s;
    SDL_UnlockMutex(b.lock);
}

/* One compiler process at a time per worker, until the sources run out. */
static int SDLCALL compile_worker(void *slot_ptr) {
    int slot = (int)(intptr_t)slot_ptr;
    char inc[1024], gen[1024], src[1024], obj[1024];
    kit_path(inc, sizeof inc, "kit/include");
    data_path(gen, sizeof gen, "game/build/gen");
    for (int i; !SDL_GetAtomicInt(&b.failed) && (i = SDL_AddAtomicInt(&b.next, 1)) < b.source_count;) {
        snprintf(src, sizeof src, "%s/%s", gen, b.sources[i]);
        data_path(obj, sizeof obj, "game/build/obj/");
        snprintf(obj + strlen(obj), sizeof obj - strlen(obj), "%.*s.o", (int)strlen(b.sources[i]) - 2, b.sources[i]);
        /* The flags of the developer build's game library (CMakeLists.txt), without debug info. */
        const char *args[32];
        int k = cc_command(args);
        static const char *const flags[] = {"-std=c11", "-O1", "-w", "-fwrapv", "-DNDEBUG", "-DEGG_SINGLE_SCREEN=1",
                                            "-DRT_GAME_LIB",
#ifndef _WIN32
                                            "-fPIC",
#endif
                                            NULL};
        for (int f = 0; flags[f]; f++) args[k++] = flags[f];
        /* zig cc makes any -O clang's -O2, which took 757 s here against 152 s for -O1 (8 threads) */
        if (b.zig) args[k++] = "-Xclang", args[k++] = "-O1";
        args[k++] = "-I", args[k++] = inc, args[k++] = "-I", args[k++] = gen;
        args[k++] = "-c", args[k++] = src, args[k++] = "-o", args[k++] = obj, args[k] = NULL;
        if (run(args, slot)) SDL_SetAtomicInt(&b.failed, 1);
        else SDL_AddAtomicInt(&b.compiled, 1);
    }
    return 0;
}

static void remove_tree(const char *dir);

static SDL_EnumerationResult SDLCALL remove_entry(void *user, const char *dir, const char *name) {
    (void)user;
    char p[1024];
    SDL_PathInfo info;
    snprintf(p, sizeof p, "%s%s", dir, name);
    if (SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_DIRECTORY) remove_tree(p);
    else SDL_RemovePath(p);
    return SDL_ENUM_CONTINUE;
}

static void remove_tree(const char *dir) {
    SDL_EnumerateDirectory(dir, remove_entry, NULL);
    SDL_RemovePath(dir);
}

/* The whole build (its own thread); sets b.failed or writes the library. */
static int SDLCALL build_thread(void *unused) {
    (void)unused;
    char dsd[1024], recomp[1024], kit[1024], work[1024], extract[1024], gen[1024], obj[1024];
    kit_path(dsd, sizeof dsd, "kit/dsd/dsd" EXE);
    kit_path(recomp, sizeof recomp, "kit/recomp/recomp.py");
    kit_path(kit, sizeof kit, "kit");
    data_path(work, sizeof work, "game/build");
    data_path(extract, sizeof extract, "game/build/extract");
    data_path(gen, sizeof gen, "game/build/gen");
    data_path(obj, sizeof obj, "game/build/obj");
    remove_tree(work);
    SDL_CreateDirectory(obj);

    set_step("Reading the game from the ROM");
    const char *ex[] = {dsd, "rom", "extract", "--rom", b.rom, "--output-path", extract, NULL};
    if (run(ex, 0)) goto fail;
    set_step("Translating the game's code to C");
    const char *rc[] = {b.python, "-I", recomp, kit, gen, "--no-comments", "--extract", extract,
                        "--region", rt_rom.code, NULL};
    if (run(rc, 0)) goto fail;

    set_step("Compiling the game");
    b.sources = SDL_GlobDirectory(gen, "*.c", 0, &b.source_count);
    if (!b.sources || !b.source_count) goto fail;
    int jobs = SDL_clamp(SDL_GetNumLogicalCPUCores(), 1, MAX_JOBS);
    SDL_Thread *workers[MAX_JOBS];
    for (int i = 0; i < jobs; i++) workers[i] = SDL_CreateThread(compile_worker, "compile", (void *)(intptr_t)i);
    for (int i = 0; i < jobs; i++) SDL_WaitThread(workers[i], NULL);
    if (SDL_GetAtomicInt(&b.failed)) goto fail;

    set_step("Linking the game");
    char lib[1024];
    data_path(lib, sizeof lib, GAME_LIBRARY);
    const char **link = SDL_calloc((size_t)b.source_count + 16, sizeof *link);
    char (*objs)[1024] = SDL_calloc((size_t)b.source_count, sizeof *objs);
    int k = cc_command(link);
    link[k++] = "-shared", link[k++] = "-o", link[k++] = lib;
    if (b.zig) link[k++] = "-nostdlib"; /* no glibc stubs to build: the executable has it all */
    for (int i = 0; i < b.source_count; i++) {
        snprintf(objs[i], sizeof objs[i], "%s/%.*s.o", obj, (int)strlen(b.sources[i]) - 2, b.sources[i]);
        link[k++] = objs[i];
    }
#ifdef _WIN32
    char implib[1024];
    kit_path(implib, sizeof implib, "kit/lib/libeggoftime.a");
    link[k++] = implib;
#endif
    int linked = run(link, 0) == 0;
    SDL_free(link), SDL_free(objs);
    if (!linked) goto fail;
    remove_tree(work);
    SDL_SetAtomicInt(&b.done, 1);
    return 0;
fail:
    SDL_SetAtomicInt(&b.failed, 1);
    SDL_SetAtomicInt(&b.done, 1);
    return 0;
}

/* Builds the game library with a progress screen; 1 when built, 0 if it failed (told) or the player quit. */
static int build(const char *log_path, const char *rom_path) {
    memset(&b, 0, sizeof b);
    if (!find_tools()) return 0;
    b.lock = SDL_CreateMutex();
    char game_dir[1024];
    data_path(game_dir, sizeof game_dir, "game");
    SDL_CreateDirectory(game_dir);
    b.log = SDL_IOFromFile(log_path, "w");
    Uint64 start = SDL_GetTicks();
    snprintf(b.rom, sizeof b.rom, "%s", rom_path);
    SDL_Thread *t = SDL_CreateThread(build_thread, "build", NULL);
    int quit = 0;
    while (!SDL_GetAtomicInt(&b.done)) {
        if (!quit && !pump(NULL)) { /* stop the tools, then leave */
            quit = 1;
            SDL_SetAtomicInt(&b.cancel, 1), SDL_SetAtomicInt(&b.failed, 1);
            SDL_LockMutex(b.lock);
            for (int i = 0; i < MAX_JOBS; i++) if (b.running[i]) SDL_KillProcess(b.running[i], true);
            SDL_UnlockMutex(b.lock);
        }
        SDL_LockMutex(b.lock);
        const char *step = b.step ? b.step : "";
        SDL_UnlockMutex(b.lock);
        char count[64] = "";
        float progress = 0.05f;
        if (b.source_count) {
            snprintf(count, sizeof count, "%d / %d", SDL_GetAtomicInt(&b.compiled), b.source_count);
            progress = 0.15f + 0.8f * (float)SDL_GetAtomicInt(&b.compiled) / (float)b.source_count;
        }
        const char *lines[] = {"Building Egg of Time from your ROM", "(first start only; this takes a few minutes)", "",
                               step, count};
        show(lines, 5, progress);
        SDL_Delay(33);
    }
    SDL_WaitThread(t, NULL);
    char line[128];
    int n = snprintf(line, sizeof line, "build %s after %u s\n", SDL_GetAtomicInt(&b.failed) ? "failed" : "done",
                     (unsigned)((SDL_GetTicks() - start) / 1000));
    log_text(line, (size_t)n);
    rt_log("%.*s", n - 1, line);
    SDL_CloseIO(b.log);
    if (b.sources) SDL_free(b.sources);
    SDL_DestroyMutex(b.lock);
    if (quit) return 0;
    if (SDL_GetAtomicInt(&b.failed)) {
        char msg[1400];
        snprintf(msg, sizeof msg, "Building the game failed.\nThe details are in %s", log_path);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Egg of Time", msg, NULL);
        return 0;
    }
    return 1;
}

/* ---- entry ---- */

int game_build_prepare(const char *rom_arg) {
    char kit_id_path[1024], lib[1024], id_path[1024], log_path[1024];
    kit_path(kit_id_path, sizeof kit_id_path, "kit/kit.id");
    data_path(lib, sizeof lib, GAME_LIBRARY);
    data_path(id_path, sizeof id_path, "game/build.id");
    data_path(log_path, sizeof log_path, "game/build.log");
    size_t kit_len = 0;
    char *kit_id = SDL_LoadFile(kit_id_path, &kit_len); /* a release: the kit's id */
    if (!load_rom(rom_arg, kit_id != NULL)) {
        SDL_free(kit_id);
        return 0;
    }
    if (kit_id) {
        /* Translation mods patch the verified ROM in memory; rom/CT.nds stays the unmodified dump. */
        char err[700];
        int translated = translation_apply(err, sizeof err);
        if (translated < 0) {
            SDL_free(kit_id);
            char msg[1600];
            snprintf(msg, sizeof msg, "A translation patch in mods/translation could not be applied.\n\n%s", err);
            fprintf(stderr, "%s\n", msg);
            rt_log("translation: %s", err);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Egg of Time", msg, NULL);
            return 0;
        }
        char patched_sha[41];
        if (translated) sha1_hex(rt_rom.data, rt_rom.size, patched_sha);
        char want[256];
        if (translated && strcmp(patched_sha, rom_sha) != 0)
            snprintf(want, sizeof want, "%.*s\n%s\n%s\n", (int)strcspn(kit_id, "\r\n"), kit_id, rom_sha, patched_sha);
        else
            snprintf(want, sizeof want, "%.*s\n%s\n", (int)strcspn(kit_id, "\r\n"), kit_id, rom_sha);
        SDL_free(kit_id);
        char *have = SDL_LoadFile(id_path, NULL);
        SDL_PathInfo info;
        int current = have && !strcmp(have, want) && SDL_GetPathInfo(lib, &info);
        SDL_free(have);
        if (!current) {
            SDL_RemovePath(id_path);
            /* dsd reads the ROM from a file: write the patched copy next to the build and delete it afterwards. */
            char patched_file[1024] = "";
            const char *rom_file = rt_rom.path;
            if (translated) {
                char game_dir[1024];
                data_path(game_dir, sizeof game_dir, "game");
                SDL_CreateDirectory(game_dir);
                data_path(patched_file, sizeof patched_file, "game/translation.nds");
                if (!SDL_SaveFile(patched_file, rt_rom.data, rt_rom.size)) {
                    char msg[1400];
                    snprintf(msg, sizeof msg, "Cannot write %s: %s", patched_file, SDL_GetError());
                    fprintf(stderr, "%s\n", msg);
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Egg of Time", msg, NULL);
                    return 0;
                }
                rom_file = patched_file;
            }
            int built = build(log_path, rom_file);
            if (patched_file[0]) SDL_RemovePath(patched_file);
            if (!built) return 0;
            SDL_SaveFile(id_path, want, strlen(want));
        }
    }
    const char *error = NULL;
    if (!rt_game_load(lib, &error)) {
        char msg[1400];
        snprintf(msg, sizeof msg, "Cannot load the game code (%s):\n%s", lib, error);
        fprintf(stderr, "%s\n", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Egg of Time", msg, NULL);
        return 0;
    }
    return 1;
}
