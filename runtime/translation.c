/* Translation mods: ROM patches in mods/translation applied to the verified ROM before the game is built.
 *
 * Patches are applied in file-name order, each to the result of the one before, so a translation split into several
 * files works and a base patch followed by a fix one applies in that order. IPS and BPS are decoded here
 * (runtime/rompatch.c); xdelta/VCDIFF patches go through the bundled xdelta3 (kit/xdelta3) because modern encoders
 * often compress their sections with LZMA, or use VCDIFF features a small decoder would not cover. xdelta 1 patches
 * (.delta, which xdelta3 cannot read) go through kit/recomp/xdelta1.py, run by the Python that builds the game.
 * When xdelta3 refuses a patch, kit/recomp/vcdiff.py tries it too: many patches were made from the dump as ndstool
 * repacks it, which that script rebuilds (it also stands in for a missing xdelta3, for patches it can decode).
 *
 * The ROM file itself is never changed: rt_rom.data holds the patched copy, while rt_rom.path/save_path stay on
 * rom/CT.nds so saves keep their place. Only release builds apply patches (game_build.c calls this when kit/kit.id
 * exists); a developer build runs its own generated code and ignores them.
 *
 * Patches made for another version of the game (Chrono Trigger DS (Japan), say) cannot be applied to the USA ROM the
 * port runs. With the player's dump of that version in mods/translation too (any name ending in .nds), the patches
 * are applied to it instead, and kit/recomp/retarget.py carries what they changed (text, fonts, graphics) over to
 * the verified ROM; it explains what cannot be carried over (a change to the game's code).
 *
 * xdelta3 and the scripts work on files, not memory: a ROM a patch changed is written to
 * game/.translation-source.nds for them, and they write game/.translation-target.nds, which is read back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "rompatch.h"
#include "rt.h"
#include "translation.h"

#ifdef _WIN32
#define XDELTA_NAME "xdelta3.exe"
#define PYTHON_TOOL "kit/python/python.exe"
#else
#define XDELTA_NAME "xdelta3"
#define PYTHON_TOOL "python3" /* from PATH, as for the build (game_build.c) */
#endif

#define PATCH_DIR "mods/translation/"
#define XDELTA_TOOL "kit/xdelta3/" XDELTA_NAME
#define XDELTA1_SCRIPT "kit/recomp/xdelta1.py"
#define VCDIFF_SCRIPT "kit/recomp/vcdiff.py"
#define RETARGET_SCRIPT "kit/recomp/retarget.py"

typedef struct PatchList {
    char **names; /* sorted file names */
    int count, cap;
} PatchList;

/* What mods/translation holds: patches, and the ROM of the version they were made for, if not the port's. */
typedef struct Listing {
    PatchList patches, roms;
} Listing;

static int is_patch(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return !SDL_strcasecmp(dot, ".ips") || !SDL_strcasecmp(dot, ".bps") || !SDL_strcasecmp(dot, ".xdelta") ||
           !SDL_strcasecmp(dot, ".vcdiff") || !SDL_strcasecmp(dot, ".xdelta3") || !SDL_strcasecmp(dot, ".delta");
}

static SDL_EnumerationResult SDLCALL add_file(void *user, const char *dir, const char *name) {
    (void)dir;
    Listing *found = user;
    const char *dot = strrchr(name, '.');
    PatchList *list = is_patch(name) ? &found->patches : dot && !SDL_strcasecmp(dot, ".nds") ? &found->roms : NULL;
    if (!list) return SDL_ENUM_CONTINUE;
    if (list->count == list->cap) {
        int cap = list->cap ? list->cap * 2 : 8;
        char **names = realloc(list->names, (size_t)cap * sizeof *names);
        if (!names) return SDL_ENUM_FAILURE;
        list->names = names, list->cap = cap;
    }
    list->names[list->count] = SDL_strdup(name);
    if (!list->names[list->count]) return SDL_ENUM_FAILURE;
    list->count++;
    return SDL_ENUM_CONTINUE;
}

static void free_list(PatchList *list) {
    for (int i = 0; i < list->count; i++) SDL_free(list->names[i]);
    free(list->names);
    memset(list, 0, sizeof *list);
}

static int compare_names(const void *a, const void *b) {
    const char *const *x = a, *const *y = b;
    int c = SDL_strcasecmp(*x, *y);
    return c ? c : strcmp(*x, *y);
}

/* The bundled tool when the release has one, else xdelta3 from PATH (a source build). */
static void find_xdelta(char *out, size_t n, const char *base) {
    snprintf(out, n, "%s%s", base, XDELTA_TOOL);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(out, &info)) snprintf(out, n, "%s", XDELTA_NAME);
}

/* A ROM-sized file read into a malloc'd buffer (the ROM data is malloc'd, so it is replaced with the same). */
static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *data = malloc(n ? (size_t)n : 1);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return data;
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int written = fwrite(data, 1, size, f) == size;
    int closed = fclose(f) == 0;
    return written && closed;
}

/* Runs a patch tool (args[0]); 0 with its output in err when it fails. The scripts (quiet) explain a failure
 * themselves, so err gets only that, and what they print on success goes to the log. */
static int run_tool(const char *const *args, int quiet, char *err, size_t err_size) {
    const char *tool = args[0];
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!p) {
        snprintf(err, err_size, "cannot start %s: %s", tool, SDL_GetError());
        return 0;
    }
    size_t log_size = 0;
    int code = -1;
    char *log = SDL_ReadProcess(p, &log_size, &code);
    SDL_DestroyProcess(p);
    if (code != 0) {
        int n = (int)SDL_min(log_size, (size_t)600);
        while (n > 0 && (log[n - 1] == '\n' || log[n - 1] == '\r')) n--;
        if (quiet && n > 0)
            snprintf(err, err_size, "%.*s", n, log);
        else
            snprintf(err, err_size, "%s failed (exit code %d)%.*s", tool, code, n, log ? log : "");
        SDL_free(log);
        return 0;
    }
    for (size_t at = 0; quiet && log && at < log_size;) {
        size_t n = strcspn(log + at, "\r\n");
        if (n) rt_log("translation: %.*s", (int)n, log + at);
        at += n + 1;
    }
    SDL_free(log);
    return 1;
}

/* The Python that runs the kit's scripts: the kit's on Windows, python3 from PATH elsewhere. */
static void find_python(char *out, size_t n, const char *base) {
#ifdef _WIN32
    snprintf(out, n, "%s%s", base, PYTHON_TOOL);
#else
    (void)base;
    snprintf(out, n, "%s", PYTHON_TOOL);
#endif
}

/* A ROM being patched: its bytes (malloc'd), and the file that holds them while no patch has changed them. */
typedef struct PatchedRom {
    uint8_t *data;
    size_t size;
    const char *file;
    int dirty;
} PatchedRom;

/* Applies the listed patches to rom in order; 0, or -1 with a message in err. */
static int apply_patches(const char *base, const char *dir, const PatchList *list, PatchedRom *rom, char *err,
                         size_t err_size) {
    char tool[1024] = "", python[1024] = "", script[1024], game_dir[1024], src_tmp[1024], dst_tmp[1024];
    snprintf(game_dir, sizeof game_dir, "%sgame", base);
    snprintf(src_tmp, sizeof src_tmp, "%s/.translation-source.nds", game_dir);
    snprintf(dst_tmp, sizeof dst_tmp, "%s/.translation-target.nds", game_dir);
    for (int i = 0; i < list->count; i++) {
        char path[1024], why[700];
        snprintf(path, sizeof path, "%s%s", dir, list->names[i]);
        size_t patch_size = 0;
        uint8_t *patch = SDL_LoadFile(path, &patch_size);
        if (!patch) {
            snprintf(err, err_size, "%s: %s", list->names[i], SDL_GetError());
            return -1;
        }
        RomPatchFormat format = rompatch_format(patch, patch_size);
        uint8_t *out = NULL;
        size_t out_size = 0;
        if (format == ROM_PATCH_IPS || format == ROM_PATCH_BPS) {
            int ok = rompatch_apply(format, patch, patch_size, rom->data, rom->size, &out, &out_size, why,
                                    sizeof why);
            SDL_free(patch);
            if (!ok) {
                snprintf(err, err_size, "%s: %s", list->names[i], why);
                return -1;
            }
        } else if (format == ROM_PATCH_XDELTA || format == ROM_PATCH_XDELTA1) {
            SDL_free(patch);
            if (format == ROM_PATCH_XDELTA && !tool[0]) find_xdelta(tool, sizeof tool, base);
            if (format == ROM_PATCH_XDELTA1 && !python[0]) {
                find_python(python, sizeof python, base);
                snprintf(script, sizeof script, "%s%s", base, XDELTA1_SCRIPT);
            }
            SDL_CreateDirectory(game_dir);
            const char *source = rom->file;
            if (rom->dirty) {
                if (!write_file(src_tmp, rom->data, rom->size)) {
                    snprintf(err, err_size, "%s: cannot write %s", list->names[i], src_tmp);
                    return -1;
                }
                source = src_tmp;
            }
            SDL_RemovePath(dst_tmp);
            const char *xdelta_args[] = {tool, "-d", "-f", "-s", source, path, dst_tmp, NULL};
            const char *xdelta1_args[] = {python, "-I", script, path, source, dst_tmp, NULL};
            int ok = format == ROM_PATCH_XDELTA ? run_tool(xdelta_args, 0, why, sizeof why)
                                                : run_tool(xdelta1_args, 1, why, sizeof why);
            if (!ok && format == ROM_PATCH_XDELTA) {
                /* Made from an ndstool-repacked dump, or no xdelta3: kit/recomp/vcdiff.py tries both. */
                char vcdiff[1024], why2[700];
                if (!python[0]) find_python(python, sizeof python, base);
                snprintf(vcdiff, sizeof vcdiff, "%s%s", base, VCDIFF_SCRIPT);
                const char *vcdiff_args[] = {python, "-I", vcdiff, path, source, dst_tmp, NULL};
                SDL_RemovePath(dst_tmp);
                ok = run_tool(vcdiff_args, 1, why2, sizeof why2);
                if (!ok && !strncmp(why, "cannot start", 12))
                    snprintf(why, sizeof why, "%s", why2); /* no xdelta3: the script's reason is the one */
                else if (!ok)
                    rt_log("translation: %s: %s", list->names[i], why2);
            }
            out = ok ? read_file(dst_tmp, &out_size) : NULL;
            SDL_RemovePath(dst_tmp);
            if (rom->dirty) SDL_RemovePath(src_tmp);
            if (!ok) {
                snprintf(err, err_size, "%s: %s", list->names[i], why);
                return -1;
            }
            if (!out || out_size < 0x200) {
                free(out);
                snprintf(err, err_size, "%s: the patch tool wrote no usable ROM", list->names[i]);
                return -1;
            }
        } else {
            SDL_free(patch);
            snprintf(err, err_size, "%s: not an IPS, BPS, xdelta or xdelta 1 patch", list->names[i]);
            return -1;
        }
        free(rom->data);
        rom->data = out;
        rom->size = out_size;
        rom->dirty = 1;
        rt_log("translation: applied %s", list->names[i]);
    }
    return 0;
}

/* Carries the patched other version (rom, made from the dump at original) over to the verified ROM with
 * kit/recomp/retarget.py; its result replaces rt_rom.data. 0, or -1 with a message in err. */
static int retarget(const char *base, const char *original, const PatchedRom *rom, char *err, size_t err_size) {
    char python[1024], script[1024], game_dir[1024], src_tmp[1024], dst_tmp[1024], why[700];
    find_python(python, sizeof python, base);
    snprintf(script, sizeof script, "%s%s", base, RETARGET_SCRIPT);
    snprintf(game_dir, sizeof game_dir, "%sgame", base);
    SDL_CreateDirectory(game_dir);
    snprintf(src_tmp, sizeof src_tmp, "%s/.translation-source.nds", game_dir);
    snprintf(dst_tmp, sizeof dst_tmp, "%s/.translation-target.nds", game_dir);
    if (!write_file(src_tmp, rom->data, rom->size)) {
        snprintf(err, err_size, "cannot write %s", src_tmp);
        return -1;
    }
    SDL_RemovePath(dst_tmp);
    const char *args[] = {python, "-I", script, rt_rom.path, original, src_tmp, dst_tmp, NULL};
    int ok = run_tool(args, 1, why, sizeof why);
    size_t out_size = 0;
    uint8_t *out = ok ? read_file(dst_tmp, &out_size) : NULL;
    SDL_RemovePath(src_tmp);
    SDL_RemovePath(dst_tmp);
    if (!ok || !out) {
        free(out);
        snprintf(err, err_size, "%s", ok ? "the translation could not be carried over" : why);
        return -1;
    }
    free(rt_rom.data);
    rt_rom.data = out;
    rt_rom.size = out_size;
    return 0;
}

int translation_apply(const char *base, char *err, size_t err_size) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s%s", base, PATCH_DIR);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(dir, &info) || info.type != SDL_PATHTYPE_DIRECTORY) return 0;

    Listing found = {0};
    PatchedRom rom = {rt_rom.data, rt_rom.size, rt_rom.path, 0};
    char original[1024] = "";
    int result = 0;
    if (!SDL_EnumerateDirectory(dir, add_file, &found)) {
        snprintf(err, err_size, "%s: %s", dir, SDL_GetError());
        result = -1;
        goto done;
    }
    if (!found.patches.count) goto done;
    qsort(found.patches.names, (size_t)found.patches.count, sizeof *found.patches.names, compare_names);
    if (found.roms.count > 1) {
        snprintf(err, err_size,
                 "there is more than one ROM (.nds) in mods/translation: keep the one the patches were made for");
        result = -1;
        goto done;
    }
    if (found.roms.count) {
        /* The version the patches were made for: they go to it, and what they change is carried over. */
        snprintf(original, sizeof original, "%s%s", dir, found.roms.names[0]);
        size_t size = 0;
        uint8_t *data = read_file(original, &size);
        if (!data || size < 0x200) {
            free(data);
            snprintf(err, err_size, "%s: cannot read the ROM", found.roms.names[0]);
            result = -1;
            goto done;
        }
        if (!memcmp(data + 0xC, rt_rom.data + 0xC, 4)) {
            free(data); /* the port's own version: the patches go to the verified ROM */
            rt_log("translation: %s is the game's own version, not used", found.roms.names[0]);
            original[0] = 0;
        } else {
            rom = (PatchedRom){data, size, original, 0};
            rt_log("translation: patching %s (%.4s)", found.roms.names[0], (const char *)data + 0xC);
        }
    }
    result = apply_patches(base, dir, &found.patches, &rom, err, err_size);
    if (original[0]) {
        if (!result) result = retarget(base, original, &rom, err, err_size);
        free(rom.data);
    } else {
        rt_rom.data = rom.data;
        rt_rom.size = rom.size;
        if (result && strstr(err, "another ROM")) {
            size_t n = strlen(err);
            snprintf(err + n, err_size - n,
                     "\n\nIf the patches were made for another version of the game (Japan), put your dump of that "
                     "version in mods/translation too (a .nds file).");
        }
    }
    if (!result) result = 1;
done:
    free_list(&found.patches);
    free_list(&found.roms);
    return result;
}
