/* Address -> recompiled function tables (generated in gen/dispatch.c). */
#pragma once

#include "recomp.h"

typedef void (*RtFunc)(CPU *cpu);

typedef struct RtFuncEntry {
    uint32_t addr;
    uint32_t sig_len;  /* code bytes covered by sig */
    uint32_t sig;      /* FNV-1a of those bytes: identifies which overlay is resident */
    RtFunc fn;
} RtFuncEntry;

typedef struct RtModule {
    const char *name;
    int overlay_id; /* -1 for always-resident modules */
    uint32_t base, end;
    const RtFuncEntry *funcs; /* sorted by addr */
    unsigned count;
    /* Modules translated as one function (code copied at run time): enter at any instruction. */
    void (*any)(CPU *cpu, uint32_t entry);
} RtModule;

/* A game function the runtime overrides with `wrap` (tools/recomp/overrides.txt) and still calls. */
typedef struct RtWrapped {
    const char *module;
    uint32_t addr;
    RtFunc fn;
} RtWrapped;

/* What the game library (gen/dispatch.c) exports as RT_GAME_SYMBOL for the runtime. */
typedef struct RtGame {
    uint32_t api_version; /* RT_API_VERSION it was built with */
    const char *game_code; /* the version of the game it was recompiled from (YQUE, YQUP, YQUJ) */
    const RtModule *modules;
    unsigned module_count;
    uint32_t entry_point; /* ARM9 entry address it was recompiled from */
    const RtWrapped *wrapped;
    unsigned wrapped_count;
} RtGame;

#define RT_GAME_SYMBOL "egg_game"
#ifdef _WIN32
#define RT_GAME_EXPORT __declspec(dllexport)
#else
#define RT_GAME_EXPORT __attribute__((visibility("default")))
#endif

/* The loaded game library (rt_game_load). */
extern const RtGame *rt_game;
/* Loads the game library at path; 0 (with *error set) if it is missing or built for another runtime or version of
 * the game than the ROM. */
int rt_game_load(const char *path, const char **error);
/* Calls the recompiled original of a `wrap` override. */
void rt_call_wrapped(CPU *cpu, const char *module, uint32_t addr);

/* 1 if overlay `id`'s code is in RAM now. */
int rt_overlay_resident(int id);
/* Calls the recompiled code at addr (Thumb bit ignored); returns 0 if there is none. */
int rt_dispatch(CPU *cpu, uint32_t addr);
