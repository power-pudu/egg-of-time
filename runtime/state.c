/* Save states.
 *
 * A state holds emulated state only: memory, device registers and internal device state, and for each game
 * thread its ARM registers and the address it continues at. Recompiled code keeps nothing else that matters
 * (it pushes return addresses to the emulated stack like the original), so a loaded thread re-enters the
 * generated code at that address and returns up through its emulated callers (rt_dispatch). States therefore
 * survive rebuilding the runtime or regenerating the code, as long as the tagged blocks keep their sizes.
 *
 * File: header, then blocks of {tag[32], u32 size, data}. Loading checks every block before applying any.
 * Optional blocks come last: a state saved before they existed ends before them and loads without them.
 * Saving waits for a poll where every thread can be described this way (normally within a frame). */
#include <stdio.h>
#include <stdlib.h>

#include "rt.h"
#include "widescreen.h"

#define STATE_MAGIC "EGGSTATE"
#define STATE_VERSION 1u
#define TAG_LEN 32

enum { MODE_SAVE, MODE_VERIFY, MODE_APPLY };

struct RtStateIO {
    int mode;
    FILE *f;
    int ok;
};

volatile int rt_state_request;
char rt_state_load_path[1024];

int rt_state_loading(const RtStateIO *s) { return s->mode == MODE_APPLY && s->ok; }
int rt_state_saving(const RtStateIO *s) { return s->mode == MODE_SAVE; }

void rt_state_bytes(RtStateIO *s, const char *tag, void *p, size_t n) {
    if (!s->ok) return;
    char t[TAG_LEN] = {0};
    strncpy(t, tag, TAG_LEN - 1);
    uint32_t n32 = (uint32_t)n;
    if (s->mode == MODE_SAVE) {
        if (fwrite(t, 1, TAG_LEN, s->f) != TAG_LEN || fwrite(&n32, 4, 1, s->f) != 1 || fwrite(p, 1, n, s->f) != n) s->ok = 0;
        return;
    }
    char ft[TAG_LEN];
    uint32_t fn;
    if (fread(ft, 1, TAG_LEN, s->f) != TAG_LEN || fread(&fn, 4, 1, s->f) != 1) {
        rt_log("state: file ends before block '%s'", tag);
        s->ok = 0;
        return;
    }
    if (memcmp(ft, t, TAG_LEN) != 0 || fn != n32) {
        rt_log("state: expected block '%s' (%u bytes), found '%.31s' (%u bytes): saved by an incompatible build", tag,
               n32, ft, fn);
        s->ok = 0;
        return;
    }
    if (s->mode == MODE_APPLY) {
        if (fread(p, 1, n, s->f) != n) s->ok = 0;
    } else if (fseek(s->f, (long)n, SEEK_CUR) != 0) {
        s->ok = 0;
    }
}

int rt_state_optional(RtStateIO *s, const char *tag, void *p, size_t n) {
    if (s->ok && s->mode != MODE_SAVE) {
        long at = ftell(s->f);
        if (fgetc(s->f) == EOF) { /* the file ends here: not in this state */
            clearerr(s->f);
            return 0;
        }
        fseek(s->f, at, SEEK_SET);
    }
    rt_state_bytes(s, tag, p, n);
    return s->ok;
}

static void all_state(RtStateIO *s) {
    struct { char magic[8]; uint32_t version; char game[4]; } hdr, want;
    memcpy(want.magic, STATE_MAGIC, 8);
    want.version = STATE_VERSION;
    memcpy(want.game, rt_rom.data + 0x0C, 4);
    hdr = want;
    if (s->mode == MODE_SAVE) {
        RT_STATE(s, hdr);
    } else {
        int mode = s->mode;
        s->mode = MODE_APPLY; /* the header is read in both passes */
        RT_STATE(s, hdr);
        s->mode = mode;
        if (s->ok && memcmp(&hdr, &want, sizeof hdr) != 0) {
            rt_log("state: not a save state of this game / format version");
            s->ok = 0;
        }
    }
    memory_state(s);
    cpu_state(s);
    sched_state(s);
    video_state(s);
    pxi_state(s);
    card_state(s);
    snd_state(s);
    spu_state(s);
    threads_state(s);
    ws_state(s); /* optional */
}

static int run(const char *path, int mode) {
    RtStateIO s = {mode, fopen(path, mode == MODE_SAVE ? "wb" : "rb"), 1};
    if (!s.f) {
        rt_log("state: cannot open %s", path);
        return 0;
    }
    all_state(&s);
    if (fclose(s.f) != 0) s.ok = 0;
    return s.ok;
}

/* Game thread, from rt_poll. */
void rt_state_poll(CPU *cpu) {
    if (rt_state_request == RT_STATE_SAVE) {
        /* The running thread must be at a poll with a known address, outside interrupt handlers. */
        if (cpu != rt_cur_cpu || !cpu->poll_pc || cpu->irq_depth || !rt_threads_can_save()) return;
        rt_state_request = 0;
        if (run(rt_rom.state_path, MODE_SAVE)) rt_log("state: saved %s (frame %llu)", rt_rom.state_path,
                                                   (unsigned long long)rt_frame_count);
        else rt_log("state: saving %s failed", rt_rom.state_path);
    } else if (rt_state_request == RT_STATE_LOAD) {
        rt_state_request = 0;
        if (!rt_state_load_path[0]) snprintf(rt_state_load_path, sizeof rt_state_load_path, "%s", rt_rom.state_path);
        if (!run(rt_state_load_path, MODE_VERIFY)) {
            rt_log("state: not loading %s", rt_state_load_path);
            return;
        }
        rt_threads_restart(0); /* continues in rt_state_apply_requested, on the scheduler */
    } else if (rt_state_request == RT_STATE_REBOOT) {
        rt_state_request = 0;
        rt_log("reboot");
        rt_threads_restart(1); /* continues in rt_boot, on a new boot thread */
    }
}

/* Scheduler, with every game thread stopped: apply the state checked by rt_state_poll. */
void rt_state_apply_requested(void) {
    if (!run(rt_state_load_path, MODE_APPLY)) rt_fatal(NULL, "state: loading %s failed after checking it", rt_state_load_path);
    rt_log("state: loaded %s (frame %llu)", rt_state_load_path, (unsigned long long)rt_frame_count);
}
