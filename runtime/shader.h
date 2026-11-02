/* RetroArch shader mods (shader.c): presets in mods/shaders, run on each shown screen by display.c. */
#pragma once

#include <stdint.h>

#include <SDL3/SDL.h>

/* Lists the shaders in mods/shaders (before shader_init, which may drop the Slang ones). */
void shader_scan(void);
/* After the renderer is made: 1 if it can run shaders (OpenGL 3.3); otherwise none are listed. */
int shader_init(SDL_Renderer *ren);
int shader_count(void);
const char *shader_key(int i);   /* path under mods/shaders, e.g. "slang/crt/crt-geom.slangp" (eggoftime.ini) */
const char *shader_label(int i); /* the file name without its extension */
/* Runs shader i on render target texture `in` into render target texture `out`, for screen slot 0 or 1 (each
 * keeps its own chain, so history and feedback stay per screen). 0 if it cannot run: the caller draws without. */
int shader_run(int slot, int i, SDL_Texture *in, SDL_Texture *out, uint64_t frame);
