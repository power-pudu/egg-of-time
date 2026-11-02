/* RetroArch GLSL shaders: presets (.glslp) and single shaders (.glsl), run as RetroArch's OpenGL driver runs them.
 *
 * A .glsl file holds both stages: it is compiled once with VERTEX and once with FRAGMENT defined (and
 * PARAMETER_UNIFORM, so #pragma parameter values arrive as uniforms), the defines going after its #version line.
 * Attributes: VertexCoord (the quad, 0..1), COLOR (white) and every *TexCoord (0..1: each texture is exactly its
 * image's size, so TextureSize = InputSize). Uniforms: MVPMatrix (maps 0..1 to the target), FrameCount (modulo
 * frame_count_modN), FrameDirection, OutputSize; Texture/TextureSize/InputSize for the pass's source, and the
 * same three for Orig (the chain's input), Prev and Prev1..Prev6 (earlier inputs), PassN and PassPrevN (earlier
 * passes' outputs) and each pass's aliasN; LUT textures by their name; parameters by theirs.
 *
 * Preset keys: shaders, shaderN, filter_linearN, wrap_modeN, mipmap_inputN, float_framebufferN,
 * srgb_framebufferN, frame_count_modN, aliasN, scale_typeN / scale_type_xN / scale_type_yN (source, viewport,
 * absolute), scaleN / scale_xN / scale_yN, textures (name;name) with name, name_linear, name_wrap_mode and
 * name_mipmap, parameters with their values (any parameter's name as a key sets it), and #reference "other.glslp"
 * lines (the referencing file's own keys win). Paths are relative to the file they appear in.
 *
 * Images are kept with their top at t = 0 (as SDL's textures and RetroArch's FBOs), and the last pass renders into
 * the output texture the same way up, so nothing is flipped. A last pass sized other than the output (scale set)
 * renders into its own texture, which is then stretched into the output. Unspecified filters are nearest. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "shader_gl.h" /* before rt.h: <windows.h> and recomp.h's register macros clash */
#include "rt.h"

#define MAX_PASSES 32
#define MAX_LUTS 16
#define MAX_PARAMS 256
#define MAX_REFERENCE_DEPTH 16
#define HISTORY 7 /* Prev, Prev1..Prev6 */
#define MAX_ATTRS 16

enum { SCALE_NONE, SCALE_SOURCE, SCALE_VIEWPORT, SCALE_ABSOLUTE };
enum { WRAP_BORDER, WRAP_EDGE, WRAP_REPEAT, WRAP_MIRROR, WRAP_COUNT };

typedef struct Sampling { int linear, wrap, mipmap; } Sampling;

typedef struct Pass {
    GLuint prog;
    GLint pos_attr, color_attr, tex_attrs[MAX_ATTRS];
    int ntex_attrs;
    Sampling in; /* how the pass samples its source */
    int type_x, type_y;
    float scale_x, scale_y;
    GLenum format; /* GL_RGBA8, GL_RGBA32F or GL_SRGB8_ALPHA8 */
    int frame_count_mod;
    char alias[64];
    GLuint tex, fbo; /* output */
    int w, h;
} Pass;

typedef struct Lut { char name[64]; GLuint tex; int w, h; Sampling s; } Lut;
typedef struct Param { char name[64]; float value; } Param;

struct GlslChain {
    Pass pass[MAX_PASSES];
    int npass;
    Lut lut[MAX_LUTS];
    int nlut;
    Param param[MAX_PARAMS];
    int nparam;
    int history; /* earlier inputs the shaders use (Prev..PrevN), 0 when none */
    GLuint hist_tex[HISTORY], hist_fbo;
    int hist_w, hist_h, hist_at; /* hist_at: the next slot written */
    GLuint vao, vbo, out_fbo, read_fbo;
    GLuint samplers[2][WRAP_COUNT][2];
};

/* ---- preset files ---- */

typedef struct Kv { char *key, *value, *dir; } Kv; /* dir: the folder of the file the key is from */
typedef struct Preset { Kv *kv; int n, cap; } Preset;

static void preset_free(Preset *p) {
    for (int i = 0; i < p->n; i++) SDL_free(p->kv[i].key), SDL_free(p->kv[i].value), SDL_free(p->kv[i].dir);
    SDL_free(p->kv);
}

static void kv_set(Preset *p, const char *key, const char *value, const char *dir) {
    for (int i = 0; i < p->n; i++) {
        if (!strcmp(p->kv[i].key, key)) {
            SDL_free(p->kv[i].value), SDL_free(p->kv[i].dir);
            p->kv[i].value = SDL_strdup(value), p->kv[i].dir = SDL_strdup(dir);
            return;
        }
    }
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->kv = SDL_realloc(p->kv, sizeof *p->kv * p->cap);
    }
    p->kv[p->n++] = (Kv){SDL_strdup(key), SDL_strdup(value), SDL_strdup(dir)};
}

static const Kv *kv_get(const Preset *p, const char *fmt, ...) RT_PRINTF(2, 3);
static const Kv *kv_get(const Preset *p, const char *fmt, ...) {
    char key[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(key, sizeof key, fmt, ap);
    va_end(ap);
    for (int i = 0; i < p->n; i++) {
        if (!strcmp(p->kv[i].key, key)) return &p->kv[i];
    }
    return NULL;
}

static void dir_of(const char *path, char *dir, size_t n) {
    snprintf(dir, n, "%s", path);
    char *slash = SDL_max(strrchr(dir, '/'), strrchr(dir, '\\'));
    if (slash) slash[1] = 0;
    else dir[0] = 0;
}

/* `rel` from a file in `dir`; absolute paths as they are. */
static void join_path(char *out, size_t n, const char *dir, const char *rel) {
    int absolute = rel[0] == '/' || rel[0] == '\\' || (rel[0] && rel[1] == ':');
    snprintf(out, n, "%s%s", absolute ? "" : dir, rel);
    for (char *s = out; *s; s++) {
        if (*s == '\\') *s = '/';
    }
}

/* A value as RetroArch's config files hold it: the quoted text, else the first word. */
static char *value_of(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '"') {
        char *end = strchr(++s, '"');
        if (end) *end = 0;
        return s;
    }
    s[strcspn(s, " \t\r")] = 0;
    return s;
}

static int preset_parse(Preset *p, const char *path, int depth, char *err, size_t n) {
    size_t size;
    char *text = SDL_LoadFile(path, &size);
    if (!text) {
        snprintf(err, n, "cannot read %s", path);
        return 0;
    }
    char dir[1024];
    dir_of(path, dir, sizeof dir);
    int ok = 1;
    for (char *line = text, *next; ok && line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        char *s = line + strspn(line, " \t");
        if (!strncmp(s, "#reference", 10)) {
            char ref[1024];
            join_path(ref, sizeof ref, dir, value_of(s + 10));
            if (depth >= MAX_REFERENCE_DEPTH) {
                snprintf(err, n, "%s: #reference nested too deep", path);
                ok = 0;
            } else {
                ok = preset_parse(p, ref, depth + 1, err, n);
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (*s == '#' || !eq) continue;
        char *key_end = eq;
        while (key_end > s && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
        *key_end = 0;
        if (*s) kv_set(p, s, value_of(eq + 1), dir);
    }
    SDL_free(text);
    return ok;
}

static int is_true(const Kv *kv) { return kv && (!strcmp(kv->value, "true") || !strcmp(kv->value, "1")); }

static int wrap_mode(const Kv *kv) {
    if (!kv) return WRAP_BORDER;
    if (!strcmp(kv->value, "clamp_to_edge")) return WRAP_EDGE;
    if (!strcmp(kv->value, "repeat")) return WRAP_REPEAT;
    if (!strcmp(kv->value, "mirrored_repeat")) return WRAP_MIRROR;
    return WRAP_BORDER;
}

static int scale_type(const Kv *kv) {
    if (!kv) return SCALE_NONE;
    if (!strcmp(kv->value, "viewport")) return SCALE_VIEWPORT;
    if (!strcmp(kv->value, "absolute")) return SCALE_ABSOLUTE;
    return SCALE_SOURCE;
}

/* ---- programs ---- */

static void add_param(GlslChain *c, const char *name, float value) {
    for (int i = 0; i < c->nparam; i++) {
        if (!strcmp(c->param[i].name, name)) return;
    }
    if (c->nparam == MAX_PARAMS) return;
    snprintf(c->param[c->nparam].name, sizeof c->param[0].name, "%s", name);
    c->param[c->nparam++].value = value;
}

/* The #pragma parameter lines: NAME "description" initial minimum maximum [step]. */
static void collect_params(GlslChain *c, const char *src) {
    for (const char *s = src; (s = strstr(s, "#pragma parameter")); s++) {
        char name[64];
        float initial;
        if (sscanf(s, "#pragma parameter %63s \"%*[^\"]\" %f", name, &initial) == 2) add_param(c, name, initial);
    }
}

static GLuint compile(GLenum type, const char *head, const char *defines, const char *tail, const char *path,
                      char *err, size_t n) {
    GLuint sh = sgl.CreateShader(type);
    const char *src[3] = {head, defines, tail};
    sgl.ShaderSource(sh, 3, src, NULL);
    sgl.CompileShader(sh);
    GLint ok;
    sgl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        sgl.GetShaderInfoLog(sh, sizeof log, NULL, log);
        snprintf(err, n, "%s (%s): %s", path, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        sgl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

static int load_program(GlslChain *c, Pass *p, const char *path, char *err, size_t n) {
    size_t size;
    char *text = SDL_LoadFile(path, &size);
    if (!text) {
        snprintf(err, n, "cannot read %s", path);
        return 0;
    }
    collect_params(c, text);
    /* The defines go after #version, which must come first. */
    char *head = "", *tail = text, *v = strstr(text, "#version");
    if (v) {
        char *eol = strchr(v, '\n');
        size_t len = eol ? (size_t)(eol + 1 - text) : size;
        head = SDL_malloc(len + 2);
        memcpy(head, text, len);
        head[len] = 0;
        if (!eol) strcat(head, "\n");
        tail = text + len;
    }
    GLuint vs = compile(GL_VERTEX_SHADER, head, "#define VERTEX\n#define PARAMETER_UNIFORM\n", tail, path, err, n);
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, head, "#define FRAGMENT\n#define PARAMETER_UNIFORM\n", tail, path,
                             err, n)
                   : 0;
    if (v) SDL_free(head);
    SDL_free(text);
    if (!fs) {
        if (vs) sgl.DeleteShader(vs);
        return 0;
    }
    p->prog = sgl.CreateProgram();
    sgl.AttachShader(p->prog, vs);
    sgl.AttachShader(p->prog, fs);
    sgl.LinkProgram(p->prog);
    sgl.DeleteShader(vs);
    sgl.DeleteShader(fs);
    GLint ok, nattr;
    sgl.GetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        sgl.GetProgramInfoLog(p->prog, sizeof log, NULL, log);
        snprintf(err, n, "%s (link): %s", path, log);
        return 0;
    }
    p->pos_attr = p->color_attr = -1;
    sgl.GetProgramiv(p->prog, GL_ACTIVE_ATTRIBUTES, &nattr);
    for (GLint i = 0; i < nattr; i++) {
        char name[64];
        GLint count;
        GLenum type;
        sgl.GetActiveAttrib(p->prog, (GLuint)i, sizeof name, NULL, &count, &type, name);
        GLint loc = sgl.GetAttribLocation(p->prog, name);
        size_t len = strlen(name);
        if (loc < 0) continue;
        if (!strcmp(name, "VertexCoord")) p->pos_attr = loc;
        else if (!strcmp(name, "COLOR")) p->color_attr = loc;
        else if (len >= 8 && !strcmp(name + len - 8, "TexCoord") && p->ntex_attrs < MAX_ATTRS) p->tex_attrs[p->ntex_attrs++] = loc;
    }
    /* Earlier inputs the shader samples: Prev, Prev1..Prev6. */
    if (sgl.GetUniformLocation(p->prog, "PrevTexture") >= 0) c->history = SDL_max(c->history, 1);
    for (int k = 1; k < HISTORY; k++) {
        char name[32];
        snprintf(name, sizeof name, "Prev%dTexture", k);
        if (sgl.GetUniformLocation(p->prog, name) >= 0) c->history = SDL_max(c->history, k + 1);
    }
    return 1;
}

/* ---- textures ---- */

static void make_target(GLuint *tex, GLuint *fbo, int w, int h, GLenum format) {
    if (!*tex) sgl.GenTextures(1, tex);
    sgl.BindTexture(GL_TEXTURE_2D, *tex);
    sgl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)format, w, h, 0, GL_RGBA, format == GL_RGBA32F ? GL_FLOAT : GL_UNSIGNED_BYTE,
                   NULL);
    sgl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    sgl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (!fbo) return;
    if (!*fbo) sgl.GenFramebuffers(1, fbo);
    sgl.BindFramebuffer(GL_FRAMEBUFFER, *fbo);
    sgl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    sgl.ClearColor(0, 0, 0, 0);
    sgl.Clear(GL_COLOR_BUFFER_BIT);
}

static int load_lut(Lut *l, const char *path, char *err, size_t n) {
    SDL_Surface *img = SDL_LoadSurface(path);
    SDL_Surface *rgba = img ? SDL_ConvertSurface(img, SDL_PIXELFORMAT_ABGR8888) : NULL; /* bytes R, G, B, A */
    SDL_DestroySurface(img);
    if (!rgba) {
        snprintf(err, n, "cannot load %s (PNG or BMP): %s", path, SDL_GetError());
        return 0;
    }
    l->w = rgba->w, l->h = rgba->h;
    sgl.GenTextures(1, &l->tex);
    sgl.BindTexture(GL_TEXTURE_2D, l->tex);
    sgl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    sgl.PixelStorei(GL_UNPACK_ROW_LENGTH, rgba->pitch / 4);
    sgl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, l->w, l->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
    sgl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (l->s.mipmap) sgl.GenerateMipmap(GL_TEXTURE_2D);
    SDL_DestroySurface(rgba);
    return 1;
}

static GLuint sampler(GlslChain *c, Sampling s) {
    int linear = s.linear > 0, mip = s.mipmap != 0;
    GLuint *sm = &c->samplers[linear][s.wrap][mip];
    if (*sm) return *sm;
    static const GLint wraps[WRAP_COUNT] = {GL_CLAMP_TO_BORDER, GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
    sgl.GenSamplers(1, sm);
    sgl.SamplerParameteri(*sm, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    sgl.SamplerParameteri(*sm, GL_TEXTURE_MIN_FILTER,
                          mip ? (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST) : linear ? GL_LINEAR : GL_NEAREST);
    sgl.SamplerParameteri(*sm, GL_TEXTURE_WRAP_S, wraps[s.wrap]);
    sgl.SamplerParameteri(*sm, GL_TEXTURE_WRAP_T, wraps[s.wrap]);
    return *sm;
}

/* ---- loading ---- */

static int ends_with(const char *s, const char *end) {
    size_t a = strlen(s), b = strlen(end);
    return a >= b && !SDL_strcasecmp(s + a - b, end);
}

static int build(GlslChain *c, const Preset *p, char *err, size_t n) {
    const Kv *kv = kv_get(p, "shaders");
    c->npass = kv ? SDL_atoi(kv->value) : 0;
    if (c->npass < 1 || c->npass > MAX_PASSES) {
        snprintf(err, n, "the preset has no valid shaders count");
        return 0;
    }
    for (int i = 0; i < c->npass; i++) {
        Pass *ps = &c->pass[i];
        if (!(kv = kv_get(p, "shader%d", i))) {
            snprintf(err, n, "shader%d missing", i);
            return 0;
        }
        char path[1024];
        join_path(path, sizeof path, kv->dir, kv->value);
        if (!load_program(c, ps, path, err, n)) return 0;
        kv = kv_get(p, "filter_linear%d", i);
        ps->in.linear = kv ? is_true(kv) : -1;
        ps->in.wrap = wrap_mode(kv_get(p, "wrap_mode%d", i));
        ps->in.mipmap = is_true(kv_get(p, "mipmap_input%d", i));
        ps->format = is_true(kv_get(p, "float_framebuffer%d", i)) ? GL_RGBA32F
                     : is_true(kv_get(p, "srgb_framebuffer%d", i)) ? GL_SRGB8_ALPHA8
                                                                    : GL_RGBA8;
        ps->frame_count_mod = (kv = kv_get(p, "frame_count_mod%d", i)) ? SDL_atoi(kv->value) : 0;
        if ((kv = kv_get(p, "alias%d", i))) snprintf(ps->alias, sizeof ps->alias, "%s", kv->value);
        ps->type_x = ps->type_y = scale_type(kv_get(p, "scale_type%d", i));
        if ((kv = kv_get(p, "scale_type_x%d", i))) ps->type_x = scale_type(kv);
        if ((kv = kv_get(p, "scale_type_y%d", i))) ps->type_y = scale_type(kv);
        ps->scale_x = ps->scale_y = (kv = kv_get(p, "scale%d", i)) ? (float)SDL_atof(kv->value) : 1;
        if ((kv = kv_get(p, "scale_x%d", i))) ps->scale_x = (float)SDL_atof(kv->value);
        if ((kv = kv_get(p, "scale_y%d", i))) ps->scale_y = (float)SDL_atof(kv->value);
        if (ps->type_x == SCALE_NONE) ps->type_x = i == c->npass - 1 ? SCALE_VIEWPORT : SCALE_SOURCE;
        if (ps->type_y == SCALE_NONE) ps->type_y = i == c->npass - 1 ? SCALE_VIEWPORT : SCALE_SOURCE;
    }
    /* Parameters: the shaders' defaults, then the preset's values (by each parameter's name). */
    for (int i = 0; i < c->nparam; i++) {
        if ((kv = kv_get(p, "%s", c->param[i].name))) c->param[i].value = (float)SDL_atof(kv->value);
    }
    if ((kv = kv_get(p, "textures"))) {
        char list[1024];
        snprintf(list, sizeof list, "%s", kv->value);
        for (char *save, *name = SDL_strtok_r(list, ";", &save); name; name = SDL_strtok_r(NULL, ";", &save)) {
            if (c->nlut == MAX_LUTS) break;
            Lut *l = &c->lut[c->nlut];
            snprintf(l->name, sizeof l->name, "%s", name);
            if (!(kv = kv_get(p, "%s", name))) {
                snprintf(err, n, "texture %s has no path", name);
                return 0;
            }
            char path[1024];
            join_path(path, sizeof path, kv->dir, kv->value);
            const Kv *lin = kv_get(p, "%s_linear", name);
            l->s = (Sampling){lin ? is_true(lin) : 1, wrap_mode(kv_get(p, "%s_wrap_mode", name)),
                              is_true(kv_get(p, "%s_mipmap", name))};
            if (!load_lut(l, path, err, n)) return 0;
            c->nlut++;
        }
    }
    static const float quad[32] = {0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, /* VertexCoord */
                                   0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0}; /* TexCoord */
    sgl.GenVertexArrays(1, &c->vao);
    sgl.GenBuffers(1, &c->vbo);
    sgl.BindBuffer(GL_ARRAY_BUFFER, c->vbo);
    sgl.BufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    sgl.GenFramebuffers(1, &c->out_fbo);
    sgl.GenFramebuffers(1, &c->read_fbo);
    return 1;
}

GlslChain *glsl_chain_load(const char *path, char *err, size_t n) {
    GlslChain *c = SDL_calloc(1, sizeof *c);
    Preset p = {0};
    int ok;
    if (ends_with(path, ".glsl")) { /* a single shader: a one-pass preset */
        char dir[1024];
        dir_of(path, dir, sizeof dir);
        kv_set(&p, "shaders", "1", dir);
        kv_set(&p, "shader0", path + strlen(dir), dir);
        ok = 1;
    } else {
        ok = preset_parse(&p, path, 0, err, n);
    }
    ok = ok && build(c, &p, err, n);
    preset_free(&p);
    if (!ok) {
        glsl_chain_free(c);
        return NULL;
    }
    return c;
}

void glsl_chain_free(GlslChain *c) {
    if (!c) return;
    for (int i = 0; i < c->npass; i++) {
        if (c->pass[i].prog) sgl.DeleteProgram(c->pass[i].prog);
        if (c->pass[i].tex) sgl.DeleteTextures(1, &c->pass[i].tex);
        if (c->pass[i].fbo) sgl.DeleteFramebuffers(1, &c->pass[i].fbo);
    }
    for (int i = 0; i < c->nlut; i++) sgl.DeleteTextures(1, &c->lut[i].tex);
    for (int i = 0; i < HISTORY; i++) {
        if (c->hist_tex[i]) sgl.DeleteTextures(1, &c->hist_tex[i]);
    }
    GLuint fbos[3] = {c->hist_fbo, c->out_fbo, c->read_fbo};
    for (int i = 0; i < 3; i++) {
        if (fbos[i]) sgl.DeleteFramebuffers(1, &fbos[i]);
    }
    if (c->vbo) sgl.DeleteBuffers(1, &c->vbo);
    if (c->vao) sgl.DeleteVertexArrays(1, &c->vao);
    GLuint *sm = &c->samplers[0][0][0];
    for (size_t i = 0; i < sizeof c->samplers / sizeof *sm; i++) {
        if (sm[i]) sgl.DeleteSamplers(1, &sm[i]);
    }
    SDL_free(c);
}

/* ---- running ---- */

static int pass_size(int type, float scale, int source, int viewport) {
    float v = type == SCALE_ABSOLUTE ? scale : (type == SCALE_VIEWPORT ? viewport : source) * scale;
    return SDL_clamp((int)SDL_lroundf(v), 1, 16384);
}

/* Binds tex to the next texture unit for <prefix>Texture, and sets <prefix>TextureSize and <prefix>InputSize. */
static void bind_input(GLuint prog, int *unit, const char *prefix, GLuint tex, int w, int h, GLuint sm) {
    char name[96];
    snprintf(name, sizeof name, "%sTexture", prefix);
    GLint loc = sgl.GetUniformLocation(prog, name);
    if (loc >= 0) {
        sgl.ActiveTexture(GL_TEXTURE0 + *unit);
        sgl.BindTexture(GL_TEXTURE_2D, tex);
        sgl.BindSampler((GLuint)*unit, sm);
        sgl.Uniform1i(loc, *unit);
        ++*unit;
    }
    snprintf(name, sizeof name, "%sTextureSize", prefix);
    if ((loc = sgl.GetUniformLocation(prog, name)) >= 0) sgl.Uniform2f(loc, (float)w, (float)h);
    snprintf(name, sizeof name, "%sInputSize", prefix);
    if ((loc = sgl.GetUniformLocation(prog, name)) >= 0) sgl.Uniform2f(loc, (float)w, (float)h);
}

static void draw(GlslChain *c, const Pass *p) {
    sgl.BindBuffer(GL_ARRAY_BUFFER, c->vbo);
    if (p->pos_attr >= 0) {
        sgl.VertexAttribPointer((GLuint)p->pos_attr, 4, GL_FLOAT, GL_FALSE, 0, (void *)0);
        sgl.EnableVertexAttribArray((GLuint)p->pos_attr);
    }
    for (int i = 0; i < p->ntex_attrs; i++) {
        sgl.VertexAttribPointer((GLuint)p->tex_attrs[i], 4, GL_FLOAT, GL_FALSE, 0, (void *)(16 * sizeof(float)));
        sgl.EnableVertexAttribArray((GLuint)p->tex_attrs[i]);
    }
    if (p->color_attr >= 0) sgl.VertexAttrib4f((GLuint)p->color_attr, 1, 1, 1, 1);
    sgl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (p->pos_attr >= 0) sgl.DisableVertexAttribArray((GLuint)p->pos_attr);
    for (int i = 0; i < p->ntex_attrs; i++) sgl.DisableVertexAttribArray((GLuint)p->tex_attrs[i]);
}

void glsl_chain_run(GlslChain *c, GLuint in, int w, int h, GLuint out, int out_w, int out_h, uint64_t frame) {
    static const float mvp[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1}; /* 0..1 to the target */
    if (c->history && (c->hist_w != w || c->hist_h != h)) {
        for (int k = 0; k < HISTORY; k++) make_target(&c->hist_tex[k], &c->hist_fbo, w, h, GL_RGBA8);
        c->hist_w = w, c->hist_h = h;
    }
    sgl.BindFramebuffer(GL_FRAMEBUFFER, c->out_fbo);
    sgl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
    sgl.BindVertexArray(c->vao);
    GLuint src = in;
    int src_w = w, src_h = h, direct = 0;
    for (int i = 0; i < c->npass; i++) {
        Pass *p = &c->pass[i];
        int pw = pass_size(p->type_x, p->scale_x, src_w, out_w), ph = pass_size(p->type_y, p->scale_y, src_h, out_h);
        direct = i == c->npass - 1 && pw == out_w && ph == out_h && p->format == GL_RGBA8;
        if (!direct && (pw != p->w || ph != p->h || !p->tex)) make_target(&p->tex, &p->fbo, pw, ph, p->format);
        p->w = pw, p->h = ph;
        sgl.BindFramebuffer(GL_FRAMEBUFFER, direct ? c->out_fbo : p->fbo);
        sgl.Viewport(0, 0, pw, ph);
        if (p->format == GL_SRGB8_ALPHA8) sgl.Enable(GL_FRAMEBUFFER_SRGB);
        else sgl.Disable(GL_FRAMEBUFFER_SRGB);
        sgl.UseProgram(p->prog);
        if (p->in.mipmap) {
            sgl.BindTexture(GL_TEXTURE_2D, src);
            sgl.GenerateMipmap(GL_TEXTURE_2D);
        }
        int unit = 0;
        bind_input(p->prog, &unit, "", src, src_w, src_h, sampler(c, p->in));
        bind_input(p->prog, &unit, "Orig", in, w, h, sampler(c, c->pass[0].in));
        for (int k = 0; k < c->history; k++) {
            char prefix[16] = "Prev";
            if (k) snprintf(prefix, sizeof prefix, "Prev%d", k);
            bind_input(p->prog, &unit, prefix, c->hist_tex[(c->hist_at + HISTORY - 1 - k) % HISTORY], w, h,
                       sampler(c, c->pass[0].in));
        }
        for (int j = 0; j < i; j++) { /* earlier passes, sampled as the pass after each samples it */
            char prefix[80];
            GLuint sm = sampler(c, c->pass[j + 1].in);
            snprintf(prefix, sizeof prefix, "Pass%d", j + 1);
            bind_input(p->prog, &unit, prefix, c->pass[j].tex, c->pass[j].w, c->pass[j].h, sm);
            snprintf(prefix, sizeof prefix, "PassPrev%d", i - j);
            bind_input(p->prog, &unit, prefix, c->pass[j].tex, c->pass[j].w, c->pass[j].h, sm);
            if (c->pass[j].alias[0]) bind_input(p->prog, &unit, c->pass[j].alias, c->pass[j].tex, c->pass[j].w, c->pass[j].h, sm);
        }
        for (int k = 0; k < c->nlut; k++) {
            GLint loc = sgl.GetUniformLocation(p->prog, c->lut[k].name);
            if (loc < 0) continue;
            sgl.ActiveTexture(GL_TEXTURE0 + unit);
            sgl.BindTexture(GL_TEXTURE_2D, c->lut[k].tex);
            sgl.BindSampler((GLuint)unit, sampler(c, c->lut[k].s));
            sgl.Uniform1i(loc, unit++);
        }
        for (int k = 0; k < c->nparam; k++) {
            GLint loc = sgl.GetUniformLocation(p->prog, c->param[k].name);
            if (loc >= 0) sgl.Uniform1f(loc, c->param[k].value);
        }
        GLint loc;
        if ((loc = sgl.GetUniformLocation(p->prog, "MVPMatrix")) >= 0) sgl.UniformMatrix4fv(loc, 1, GL_FALSE, mvp);
        if ((loc = sgl.GetUniformLocation(p->prog, "FrameCount")) >= 0) {
            sgl.Uniform1i(loc, (GLint)(p->frame_count_mod > 0 ? frame % (uint64_t)p->frame_count_mod : frame % 0x7fffffff));
        }
        if ((loc = sgl.GetUniformLocation(p->prog, "FrameDirection")) >= 0) sgl.Uniform1i(loc, 1);
        if ((loc = sgl.GetUniformLocation(p->prog, "OutputSize")) >= 0) sgl.Uniform2f(loc, (float)pw, (float)ph);
        draw(c, p);
        for (int k = 0; k < unit; k++) sgl.BindSampler((GLuint)k, 0);
        src = p->tex, src_w = pw, src_h = ph;
    }
    sgl.Disable(GL_FRAMEBUFFER_SRGB);
    if (!direct) { /* the last pass, stretched into the output */
        Pass *p = &c->pass[c->npass - 1];
        sgl.BindFramebuffer(GL_READ_FRAMEBUFFER, p->fbo);
        sgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->out_fbo);
        sgl.BlitFramebuffer(0, 0, p->w, p->h, 0, 0, out_w, out_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    if (c->history) { /* this input, for the next frames' Prev */
        sgl.BindFramebuffer(GL_READ_FRAMEBUFFER, c->read_fbo);
        sgl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, in, 0);
        sgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->hist_fbo);
        sgl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->hist_tex[c->hist_at], 0);
        sgl.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        c->hist_at = (c->hist_at + 1) % HISTORY;
    }
}
