/* Shader mods: RetroArch shaders in mods/shaders, chosen with System Settings > Shader.
 *
 *   mods/shaders/glsl/   GLSL presets (.glslp) in any subfolder, single .glsl shaders in the folder itself
 *   mods/shaders/slang/  Slang presets (.slangp) in any subfolder, single .slang shaders in the folder itself
 *
 * so RetroArch's glsl-shaders and slang-shaders collections can be copied in whole (their shader files, which only
 * work through a preset, sit in subfolders and are not listed). GLSL runs here (shader_glsl.c); Slang runs in
 * librashader (the RetroArch-compatible Slang runtime), loaded at run time from the executable's folder, or the
 * system's on Linux and macOS: without it, the Slang shaders are not listed.
 *
 * Both run on the SDL renderer's own OpenGL context (display.c picks the OpenGL renderer when shaders are found):
 * the renderer's queued drawing is flushed first, and the GL state SDL relies on is put back after. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define LIBRA_RUNTIME_OPENGL
#include "third_party/librashader.h"

#include "shader_gl.h" /* before rt.h: <windows.h> and recomp.h's register macros clash */
#include "display.h"
#include "rt.h"
#include "shader.h"

#define MAX_SHADERS 4096
#define MAX_DEPTH 8
#define MAX_UNITS 32 /* texture units reset after a shader ran */

ShaderGl sgl;

typedef struct Entry { char *key, *label; int slang; } Entry;
static Entry *entries;
static int count;

typedef struct Slot { int shader; GlslChain *glsl; libra_gl_filter_chain_t libra; } Slot;
static Slot slots[2] = {{.shader = -1}, {.shader = -1}};
static int failed = -1, last_asked = -1; /* failed: the shader that did not load, not tried again while chosen */
static SDL_Renderer *renderer;
static int gl_ready;

/* ---- the list ---- */

static int ends_with(const char *s, const char *end) {
    size_t a = strlen(s), b = strlen(end);
    return a >= b && !SDL_strcasecmp(s + a - b, end);
}

typedef struct Walk { size_t base_len; int slang, depth; } Walk; /* base_len: of ".../mods/shaders/" */

static void add(const char *key, int slang) {
    if (count == MAX_SHADERS) return;
    entries = SDL_realloc(entries, sizeof *entries * (count + 1));
    const char *base = strrchr(key, '/') + 1, *dot = strrchr(base, '.');
    entries[count++] = (Entry){SDL_strdup(key), SDL_strndup(base, (size_t)(dot - base)), slang};
}

static SDL_EnumerationResult SDLCALL walk_entry(void *user, const char *dirname, const char *fname) {
    const Walk *w = user;
    char path[1024];
    snprintf(path, sizeof path, "%s%s", dirname, fname);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        if (w->depth < MAX_DEPTH && strlen(path) + 2 < sizeof path) {
            Walk sub = {w->base_len, w->slang, w->depth + 1};
            strcat(path, "/");
            SDL_EnumerateDirectory(path, walk_entry, &sub);
        }
    } else if (ends_with(fname, w->slang ? ".slangp" : ".glslp") ||
               (w->depth == 0 && ends_with(fname, w->slang ? ".slang" : ".glsl"))) {
        char *key = path + w->base_len; /* e.g. "slang/crt/crt-geom.slangp", with / separators */
        for (char *c = key; *c; c++) {
            if (*c == '\\') *c = '/';
        }
        add(key, w->slang);
    }
    return SDL_ENUM_CONTINUE;
}

static int by_key(const void *a, const void *b) {
    return SDL_strcasecmp(((const Entry *)a)->key, ((const Entry *)b)->key);
}

void shader_scan(void) {
    for (int slang = 0; slang < 2; slang++) {
        char dir[1024];
        int base = snprintf(dir, sizeof dir, "%smods/shaders/", rt_data_dir());
        snprintf(dir + base, sizeof dir - (size_t)base, "%s/", slang ? "slang" : "glsl");
        SDL_CreateDirectory(dir); /* the folders for the player to fill (archives keep no empty folders) */
        Walk w = {(size_t)base, slang, 0};
        SDL_EnumerateDirectory(dir, walk_entry, &w);
    }
    if (count) SDL_qsort(entries, (size_t)count, sizeof *entries, by_key);
}

int shader_count(void) { return count; }
const char *shader_key(int i) { return entries[i].key; }
const char *shader_label(int i) { return entries[i].label; }

static void drop_slang(void) {
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (!entries[i].slang) entries[n++] = entries[i];
        else SDL_free(entries[i].key), SDL_free(entries[i].label);
    }
    count = n;
}

/* ---- librashader ---- */

#define LIBRA_FUNCS(X)                                                                                             \
    X(instance_abi_version) X(instance_api_version) X(preset_create) X(preset_free) X(error_write)                 \
    X(error_free_string) X(error_free) X(gl_filter_chain_create) X(gl_filter_chain_frame) X(gl_filter_chain_free)
static struct {
#define X(f) PFN_libra_##f f;
    LIBRA_FUNCS(X)
#undef X
} libra;
static LIBRASHADER_API_VERSION libra_api;

static int libra_load(void) {
    static const char *const names[] = {
#if defined(_WIN32)
        "librashader.dll",
#elif defined(__APPLE__)
        "librashader.dylib",
#else
        "librashader.so", "librashader.so.2",
#endif
    };
    SDL_SharedObject *lib = NULL;
    for (size_t i = 0; !lib && i < SDL_arraysize(names) * 2; i++) { /* the executable's folder first, then the system's */
        char path[1024];
        snprintf(path, sizeof path, "%s%s", i < SDL_arraysize(names) ? rt_install_dir() : "", names[i % SDL_arraysize(names)]);
        lib = SDL_LoadObject(path);
    }
    if (!lib) return 0;
    int ok = 1;
#define X(f) ok &= (libra.f = (PFN_libra_##f)SDL_LoadFunction(lib, "libra_" #f)) != NULL;
    LIBRA_FUNCS(X)
#undef X
    if (!ok || libra.instance_abi_version() != LIBRASHADER_CURRENT_ABI) {
        rt_log("shaders: librashader found, but not a compatible version (ABI %d)", LIBRASHADER_CURRENT_ABI);
        SDL_UnloadObject(lib);
        return 0;
    }
    libra_api = SDL_min(libra.instance_api_version(), (LIBRASHADER_API_VERSION)LIBRASHADER_CURRENT_VERSION);
    return 1;
}

static int libra_failed(libra_error_t e, char *err, size_t n) {
    if (!e) return 0;
    char *msg = NULL;
    libra.error_write(e, &msg);
    snprintf(err, n, "%s", msg ? msg : "librashader error");
    libra.error_free_string(&msg);
    libra.error_free(&e);
    return 1;
}

static const void *gl_proc(const char *name) {
    SDL_FunctionPointer f = SDL_GL_GetProcAddress(name);
    const void *p;
    memcpy(&p, &f, sizeof p);
    return p;
}

/* ---- running ---- */

int shader_init(SDL_Renderer *ren) {
    renderer = ren;
    if (!count) return 0;
    const char *name = SDL_GetRendererName(ren);
    int ok = name && !strcmp(name, "opengl");
#define X(f) ok = ok && (sgl.f = (__typeof__(sgl.f))SDL_GL_GetProcAddress("gl" #f)) != NULL;
    SHADER_GL_FUNCS(X)
#undef X
    int major = 0, minor = 0;
    if (ok) sscanf((const char *)sgl.GetString(GL_VERSION), "%d.%d", &major, &minor);
    if (!ok || major * 10 + minor < 33) {
        rt_log("shaders: unavailable, they need OpenGL 3.3 (renderer: %s, OpenGL %d.%d)", name ? name : "none", major, minor);
        count = 0;
        return 0;
    }
    rt_log("shaders: OpenGL %s on %s", (const char *)sgl.GetString(GL_VERSION), (const char *)sgl.GetString(GL_RENDERER));
    int slang = 0;
    for (int i = 0; i < count; i++) slang |= entries[i].slang;
    if (slang && !libra_load()) {
        rt_log("shaders: librashader not found, so the Slang shaders are not listed");
        drop_slang();
    }
    gl_ready = count > 0;
    return gl_ready;
}

/* The GL state SDL's renderer expects that it does not set again itself (SDL_FlushRenderer resets the rest). */
static void restore_gl(void) {
    sgl.BindVertexArray(0);
    sgl.UseProgram(0);
    sgl.BindBuffer(GL_ARRAY_BUFFER, 0);
    for (GLuint u = 0; u < MAX_UNITS; u++) sgl.BindSampler(u, 0);
    sgl.ActiveTexture(GL_TEXTURE0);
    sgl.BindTexture(GL_TEXTURE_2D, 0);
    sgl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    sgl.Disable(GL_FRAMEBUFFER_SRGB);
    sgl.Disable(GL_SCISSOR_TEST);
    sgl.Disable(GL_BLEND);
    while (sgl.GetError() != GL_NO_ERROR) {}
}

static void slot_free(Slot *s) {
    glsl_chain_free(s->glsl);
    if (s->libra) libra.gl_filter_chain_free(&s->libra);
    *s = (Slot){.shader = -1};
}

static int slot_load(Slot *s, int i, char *err, size_t n) {
    char path[1024];
    snprintf(path, sizeof path, "%smods/shaders/%s", rt_data_dir(), entries[i].key);
    s->shader = i;
    if (!entries[i].slang) return (s->glsl = glsl_chain_load(path, err, n)) != NULL;
    if (ends_with(path, ".slang")) { /* librashader reads presets only: a one-pass preset naming the shader */
        const char *tmp = getenv("TEMP") ? getenv("TEMP") : getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
        char text[1100];
        int len = snprintf(text, sizeof text, "shaders = 1\nshader0 = \"%s\"\n", path);
        snprintf(path, sizeof path, "%s/eggoftime-shader.slangp", tmp);
        if (!SDL_SaveFile(path, text, (size_t)len)) {
            snprintf(err, n, "cannot write %s", path);
            return 0;
        }
    }
    libra_shader_preset_t preset = NULL;
    if (libra_failed(libra.preset_create(path, &preset), err, n)) return 0;
    filter_chain_gl_opt_t opt = {.version = libra_api, .glsl_version = 330};
    return !libra_failed(libra.gl_filter_chain_create(&preset, gl_proc, &opt, &s->libra), err, n);
}

static GLuint gl_texture(SDL_Texture *t, int *w, int *h) {
    SDL_PropertiesID props = SDL_GetTextureProperties(t);
    if (SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_OPENGL_TEXTURE_TARGET_NUMBER, 0) != GL_TEXTURE_2D) return 0;
    float fw, fh;
    SDL_GetTextureSize(t, &fw, &fh);
    *w = (int)fw, *h = (int)fh;
    return (GLuint)SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0);
}

int shader_run(int slot, int i, SDL_Texture *in, SDL_Texture *out, uint64_t frame) {
    if (i != last_asked) failed = -1, last_asked = i; /* chosen again: try again */
    if (!gl_ready || i < 0 || i >= count || i == failed) return 0;
    int w, h, out_w, out_h;
    GLuint tin = gl_texture(in, &w, &h), tout = gl_texture(out, &out_w, &out_h);
    if (!tin || !tout) return 0;
    SDL_FlushRenderer(renderer);
    Slot *s = &slots[slot];
    if (s->shader != i) {
        char err[1536] = "";
        slot_free(s);
        Uint64 t0 = SDL_GetTicks();
        if (!slot_load(s, i, err, sizeof err)) {
            rt_log("shaders: %s did not load: %s", entries[i].key, err);
            display_message("Shader failed to load (see the log)", 3000);
            slot_free(s);
            failed = i;
            restore_gl();
            return 0;
        }
        rt_log("shaders: %s loaded for screen %d (%u ms)", entries[i].key, slot, (unsigned)(SDL_GetTicks() - t0));
    }
    if (s->glsl) {
        glsl_chain_run(s->glsl, tin, w, h, tout, out_w, out_h, frame);
    } else {
        frame_gl_opt_t opt = {.version = libra_api, .frame_direction = 1, .total_subframes = 1, .current_subframe = 1,
                              .frames_per_second = 60};
        /* librashader samples with mipmap filters: one level makes the input complete for them (SDL only draws
         * into this texture, never samples it). */
        sgl.BindTexture(GL_TEXTURE_2D, tin);
        sgl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        libra_image_gl_t src = {tin, GL_RGBA8, (uint32_t)w, (uint32_t)h}, dst = {tout, GL_RGBA8, (uint32_t)out_w, (uint32_t)out_h};
        char err[1024];
        if (libra_failed(libra.gl_filter_chain_frame(&s->libra, (size_t)frame, src, dst, NULL, NULL, &opt), err, sizeof err)) {
            rt_log("shaders: %s: %s", entries[i].key, err);
            failed = i;
        }
    }
    restore_gl();
    SDL_FlushRenderer(renderer);
    return failed != i;
}
