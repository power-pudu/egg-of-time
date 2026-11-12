/* Emulated time, video timing, interrupt controller, timers and DMA.
 *
 * Emulated time follows the host clock (33.513982 MHz bus cycles since boot), times the game speed (fast
 * forward: rt_set_speed, applied by the game thread at its next look at the clock). Events are processed
 * lazily: whenever the game polls (RT_POLL at loop back-edges, BIOS waits, register reads that depend
 * on time), everything up to "now" is raised. A host timer requests a poll every millisecond. */
#include <SDL3/SDL.h>

#include "cheats.h"
#include "cheevos.h"
#include "rt.h"
#include "single_screen.h"
#include "video.h"

uint64_t rt_frame_count;

static Uint64 base_ns;     /* host time at base_cycles */
static uint64_t base_cycles;
static int speed = 1;      /* emulated time per host time */
static SDL_AtomicInt speed_wanted;
static uint32_t irq_flags;
static uint64_t line_done; /* absolute index of the last scanline whose events were raised */
/* Where video time is within line_done: its start events raised, drawn, or its HBlank events raised. Each
 * step waits for the interrupts the previous one raised (sched_update). */
enum { LINE_STARTED, LINE_DRAWN, LINE_HBLANK };
static int line_phase = LINE_HBLANK;

#define VIDEO_MAX_LAG 192u /* lines */

uint64_t sched_now(void) {
    Uint64 t = SDL_GetTicksNS();
    uint64_t now = base_cycles + (uint64_t)((double)(t - base_ns) * (RT_CLOCK_HZ / 1e9) * speed);
    /* Host stall: time more than VIDEO_MAX_LAG lines ahead of video is dropped, the game held up with it. Lines
     * stepped past pending interrupts to catch it up merged the game's HBlank handlers: the one at line 262 writes
     * line 0's entry of the software HDMA, and a frame after a stall (a screenshot written, in the Blackbird seen
     * from above) began with the text box's BG0/BG1 instead of the clouds', the box drawn into the picture and a line
     * above it. Never before an earlier now: line_done only grows. */
    uint64_t lag_end = (line_done + VIDEO_MAX_LAG) * RT_LINE_CYCLES;
    if (now > lag_end) now = lag_end, base_ns = t, base_cycles = now;
    int want = SDL_GetAtomicInt(&speed_wanted);
    if (want > 0 && want != speed) { /* time goes on from here at the new speed */
        base_ns = t;
        base_cycles = now;
        speed = want;
    }
    return now;
}

void rt_set_speed(int s) { SDL_SetAtomicInt(&speed_wanted, s); }
int sched_speed(void) { return speed; }

/* ---- interrupt controller ---- */

void irq_changed(void) { rt_poll_pending = 1; }

void irq_raise(uint32_t bits) {
    irq_flags |= bits;
    rt_poll_pending = 1;
}

uint32_t irq_if(void) {
    sched_update();
    return irq_flags;
}

void irq_ack(uint32_t bits) { irq_flags &= ~bits; }

static int irq_wanted(void) { return (io_get32(0x210) & irq_flags) != 0; }

/* ---- video timing ---- */

static uint16_t lyc(void) {
    uint16_t ds = io_get16(0x004);
    return (uint16_t)((ds >> 8) | ((ds & 0x80) << 1));
}

/* Video time is the line whose events were raised last; it trails the host clock while drawing waits for
 * an interrupt handler (sched_update). Register reads catch up first. */
uint16_t video_vcount(void) {
    sched_update();
    return (uint16_t)(line_done % RT_FRAME_LINES);
}

uint16_t video_dispstat(void) {
    uint32_t line = video_vcount();
    uint16_t ds = io_get16(0x004) & 0xFFB8u;
    if (line >= 192 && line < 262) ds |= 1;
    if (line_phase == LINE_HBLANK) ds |= 2;
    if (line == lyc()) ds |= 4;
    return ds;
}

/* X/Y buttons and lid state as the ARM7 publishes them for PAD_Read. */
#define BUTTON_XY_BUF 0x027FFFA8u
/* The VBlank count the ARM7 keeps (OS_GetVBlankCount). The game's play time goes by it: main 0x0202c95c adds the
 * seconds it advanced (count / 60) since its last call to the play time (seconds; in RAM at 0x02073204 after loading
 * a save); left at 0, the TIME stayed 00:00. */
#define VBLANK_COUNT 0x027FFC3Cu

/* Start of a line: VBlank, VCount match. */
static void video_line_start(uint64_t abs_line) {
    uint32_t line = (uint32_t)(abs_line % RT_FRAME_LINES);
    uint16_t ds = io_get16(0x004);
    if (line == 192) {
        rt_frame_count++;
        video_end_frame();
        cheevos_frame();
        cheats_frame();
        uint32_t keys = ss_game_keys(rt_keys);
        /* PAD_Read ORs this into KEYINPUT: X = bit 10, Y = bit 11, debug = bit 13 (1 = released);
         * bit 15 = lid closed. */
        rt_w16(BUTTON_XY_BUF, (uint16_t)(0x2C00u & ~(((keys & KEY_X) ? 0x400u : 0) | ((keys & KEY_Y) ? 0x800u : 0))));
        rt_w32(VBLANK_COUNT, rt_r32(VBLANK_COUNT) + 1);
        if (ds & 0x08) irq_raise(IRQ_VBLANK);
        dma_trigger(DMA_START_VBLANK);
    }
    if (line == lyc() && (ds & 0x20)) irq_raise(IRQ_VCOUNT);
}

/* HBlank at the end of a line (VCOUNT still reads that line). */
static void video_line_hblank(uint64_t abs_line) {
    if (io_get16(0x004) & 0x10) irq_raise(IRQ_HBLANK);
    if (abs_line % RT_FRAME_LINES < 192) dma_trigger(DMA_START_HBLANK);
}

/* ---- timers ---- */

typedef struct Timer {
    int running, countup, shift;
    uint16_t reload_used; /* reload value of the current period */
    uint64_t start;       /* cycle of the last reload (non-count-up timers) */
    uint32_t count;       /* current value for count-up and stopped timers */
} Timer;
static Timer timers[4];
static const int prescale_shift[4] = {0, 6, 8, 10};

static uint16_t timer_ctl(int n) { return (uint16_t)(io_get32(0x100 + 4 * n) >> 16); }
static uint16_t timer_reload(int n) { return (uint16_t)io_get32(0x100 + 4 * n); }
static uint64_t timer_period(const Timer *t) { return (uint64_t)(0x10000u - t->reload_used) << t->shift; }

static void timer_overflow(int n, uint32_t times);

static void timer_tick_countup(int n, uint32_t ticks) {
    Timer *t = &timers[n];
    if (!t->running || !t->countup) return;
    uint32_t overflows = 0;
    while (ticks) {
        uint32_t room = 0x10000u - t->count;
        if (ticks < room) { t->count += ticks; break; }
        ticks -= room;
        t->count = timer_reload(n);
        overflows++;
    }
    if (overflows) timer_overflow(n, overflows);
}

static void timer_overflow(int n, uint32_t times) {
    if (timer_ctl(n) & 0x40) irq_raise(IRQ_TIMER0 << n);
    if (n < 3) timer_tick_countup(n + 1, times);
}

static void timers_update(uint64_t now) {
    for (int n = 0; n < 4; n++) {
        Timer *t = &timers[n];
        if (!t->running || t->countup) continue;
        uint32_t overflows = 0;
        while (now >= t->start + timer_period(t)) {
            t->start += timer_period(t);
            t->reload_used = timer_reload(n);
            if (++overflows == 4096) { /* host stall: skip ahead */
                uint64_t p = timer_period(t);
                t->start += (now - t->start) / p * p;
            }
        }
        if (overflows) timer_overflow(n, overflows);
    }
}

uint16_t timer_counter(int n) {
    Timer *t = &timers[n];
    if (!t->running || t->countup) return (uint16_t)t->count;
    uint64_t now = sched_now();
    timers_update(now);
    return (uint16_t)(t->reload_used + ((now - t->start) >> t->shift));
}

void timer_control_written(int n, uint16_t old_ctl) {
    Timer *t = &timers[n];
    uint16_t ctl = timer_ctl(n);
    uint64_t now = sched_now();
    if (t->running && !t->countup) t->count = timer_counter(n);
    t->shift = prescale_shift[ctl & 3];
    t->countup = n > 0 && (ctl & 4);
    if ((ctl & 0x80) && !(old_ctl & 0x80)) {
        t->reload_used = timer_reload(n);
        t->count = t->reload_used;
        t->start = now;
    } else if ((ctl & 0x80) && !t->countup) {
        t->reload_used = (uint16_t)t->count; /* prescaler/mode change while running: continue from current value */
        t->start = now;
    }
    t->running = (ctl & 0x80) != 0;
}

/* ---- DMA ---- */

typedef struct Dma {
    uint32_t src, dst, count; /* internal registers latched at enable */
} Dma;
static Dma dmas[4];

static uint32_t dma_cnt(int ch) { return io_get32(0x0B8 + 12 * ch); }

static void dma_run(int ch) {
    Dma *d = &dmas[ch];
    uint32_t cnt = dma_cnt(ch);
    int word = (cnt >> 26) & 1;
    uint32_t unit = word ? 4 : 2;
    int dctl = (cnt >> 21) & 3, sctl = (cnt >> 23) & 3;
    int32_t dstep = dctl == 1 ? -(int32_t)unit : dctl == 2 ? 0 : (int32_t)unit;
    int32_t sstep = sctl == 1 ? -(int32_t)unit : sctl == 2 ? 0 : (int32_t)unit;
    for (uint32_t i = 0; i < d->count; i++) {
        if (word) rt_w32(d->dst, rt_r32(d->src));
        else rt_w16(d->dst, rt_r16(d->src));
        d->src += (uint32_t)sstep;
        d->dst += (uint32_t)dstep;
    }
    int mode = (cnt >> 27) & 7;
    if ((cnt & (1u << 25)) && mode != DMA_START_IMMEDIATE) {
        d->count = (cnt & 0x1FFFFF) ? (cnt & 0x1FFFFF) : 0x200000;
        if (dctl == 3) d->dst = io_get32(0x0B4 + 12 * ch);
    } else {
        io_set32(0x0B8 + 12 * ch, cnt & ~0x80000000u);
    }
    if (cnt & (1u << 30)) irq_raise(IRQ_DMA0 << ch);
}

void dma_control_written(int ch, uint32_t old_cnt) {
    uint32_t cnt = dma_cnt(ch);
    if (!(cnt & 0x80000000u) || (old_cnt & 0x80000000u)) return;
    Dma *d = &dmas[ch];
    d->src = io_get32(0x0B0 + 12 * ch);
    d->dst = io_get32(0x0B4 + 12 * ch);
    d->count = (cnt & 0x1FFFFF) ? (cnt & 0x1FFFFF) : 0x200000;
    int mode = (cnt >> 27) & 7;
    /* GX FIFO DMA: the geometry engine is not emulated yet, so the FIFO always accepts data. */
    if (mode == DMA_START_IMMEDIATE || mode == DMA_START_GXFIFO) dma_run(ch);
    else if (mode == DMA_START_CARD) card_pump_dma();
}

void dma_trigger(int start_mode) {
    for (int ch = 0; ch < 4; ch++) {
        uint32_t cnt = dma_cnt(ch);
        if ((cnt & 0x80000000u) && (int)((cnt >> 27) & 7) == start_mode) dma_run(ch);
    }
}

/* ---- scheduling ---- */

/* A frame starts drawing once the game waits for an interrupt (rt_halt), up to FRAME_HOLD_MAX lines late. The
 * game prepares a frame (sprites, registers) and then waits for VBlank; on the DS it always gets there before
 * line 0. Here emulated time follows the host clock, so a host hiccup, or fast forward halving the time a frame
 * has, can leave the game mid-update at line 0, and the frame was drawn half updated: sprites gone, the time
 * gate's OBJ window square, a BG missing (fast forward beside the gate after Lucca arrives). Held lines catch up
 * once it waits, so game speed does not change. */
#define FRAME_HOLD_MAX 128u /* lines */
static int game_waiting; /* the game thread is in rt_halt */

/* Fast forward: the game waits for VBlank discarding any already raised (OS_WaitVBlankIntr), so a frame whose
 * work is not done by line 192 loses the VBlank and a whole frame of game time. At 2x a frame has half the host
 * time, and heavy frames (map columns streamed in while walking, events) overran: walking in fast forward went
 * at 1.0-1.7x and felt slower than normal. Emulated time now holds at the end of line 191 while the game is still
 * busy, for up to the host time a frame has at normal speed (FF_HOLD_MAX_NS), and the held time is dropped, not
 * caught up: fast forward runs as fast as the host allows, up to its speed, and not below normal speed. */
#define FF_HOLD_MAX_NS (SDL_NS_PER_SECOND / 60)
static Uint64 ff_hold_since; /* host time the hold began (0: not holding) */

static int ff_hold(uint64_t *now) {
    uint64_t at = (line_done + 1) * RT_LINE_CYCLES; /* the start of the next line */
    if (speed < 2 || game_waiting || line_phase != LINE_HBLANK || (line_done + 1) % RT_FRAME_LINES != 192 || *now < at) {
        ff_hold_since = 0;
        return 0;
    }
    Uint64 t = SDL_GetTicksNS();
    if (!ff_hold_since) ff_hold_since = t;
    if (t - ff_hold_since >= FF_HOLD_MAX_NS) return 0; /* the game never waited: go on (until the next frame) */
    base_ns = t, base_cycles = at - 1; /* time stays here */
    *now = at - 1;
    return 1;
}

void sched_update(void) {
    uint64_t now = sched_now();
    ff_hold(&now);
    uint64_t line = now / RT_LINE_CYCLES;
    /* Games change video registers from VBlank/HBlank/VCount handlers in the middle of a frame. Lines are
     * drawn behind the host clock, so stop at a line whose interrupt is pending, and continue once the
     * handler has run: its register writes then reach the right line instead of one up to a poll later.
     * This includes while interrupts are masked by CPSR or IME (another handler running, SDK critical
     * sections). Video also holds while a handler runs: catching up while e.g. the VBlank handler is still
     * re-arming HBlank DMA would shift the per-line register table. Up to VIDEO_MAX_LAG lines behind the host
     * clock; at that, one line per call, so code that waits for VCOUNT with interrupts off still sees time pass, but
     * a stall is never caught up in one go inside a handler or with interrupts masked (time beyond it is dropped):
     * the HBlank interrupts of those lines merged, the game's software HDMA (HBlank handler) missed the line ending
     * the text box, and the rest of the frame was drawn with the box's one-screen BG0/BG1 and no window (the field's
     * trees and the time gate gone). */
    int stepped = 0; /* went on past a pending interrupt or a running handler (far behind) */
    for (;;) {
        int in_handler = rt_cur_cpu && (rt_cur_cpu->cpsr_ctl & 0x1F) == 0x12; /* IRQ mode */
        if (line_phase != LINE_DRAWN && (irq_wanted() || in_handler)) {
            if (line - line_done >= VIDEO_MAX_LAG && !stepped) {
                stepped = 1;
            } else {
                rt_poll_pending = 1;
                break;
            }
        }
        if (line_phase == LINE_STARTED) {
            uint32_t l = (uint32_t)(line_done % RT_FRAME_LINES);
            if (l < 192) video_render_line((int)l);
            line_phase = LINE_DRAWN;
        } else if (line_done >= line) {
            break;
        } else if (line_phase == LINE_DRAWN) {
            video_line_hblank(line_done);
            line_phase = LINE_HBLANK;
        } else if ((line_done + 1) % RT_FRAME_LINES == 0 && !game_waiting && line - line_done < FRAME_HOLD_MAX) {
            break; /* the next frame: the game is still preparing it */
        } else {
            video_line_start(++line_done);
            line_phase = LINE_STARTED;
            /* A frame started after the hold ran out (the game never waited): stop at line 0, or catching up the
             * held lines in this call passes it unseen, and code that busy-waits for VCOUNT 0 (crt0, before
             * interrupts are set up) misses it every frame and never boots. */
            if (line_done % RT_FRAME_LINES == 0 && !game_waiting) break;
        }
    }
    timers_update(now);
    pxi_update(now);
}

static SDL_AtomicInt quit; /* 1: the host asks the game thread to stop, 2: it has stopped */

/* The game thread waits here for the process to end once the host quits, so SDL_Quit does not free the audio
 * stream and the rest under it. Polls run between the game's instructions, never inside a save or state write. */
static void park_on_quit(void) {
    if (SDL_GetAtomicInt(&quit) != 1) return;
    SDL_SetAtomicInt(&quit, 2);
    for (;;) SDL_Delay(1000);
}

int rt_stop_game(uint32_t timeout_ms) {
    SDL_SetAtomicInt(&quit, 1);
    rt_poll_pending = 1;
    for (Uint64 end = SDL_GetTicks() + timeout_ms; SDL_GetAtomicInt(&quit) != 2; SDL_Delay(1)) {
        if (SDL_GetTicks() >= end) return 0;
    }
    return 1;
}

/* Requests a poll every 0.5 ms so long-running game code still sees interrupts and video timing. */
static int SDLCALL poll_ticker(void *arg) {
    (void)arg;
    for (;;) {
        SDL_DelayPrecise(500000);
        rt_poll_pending = 1;
    }
    return 0;
}

void sched_init(void) {
    base_ns = SDL_GetTicksNS();
    base_cycles = 0;
    rt_frame_count = 0;
    irq_flags = 0;
    line_done = 0;
    line_phase = LINE_HBLANK;
    memset(timers, 0, sizeof timers);
    memset(dmas, 0, sizeof dmas);
    static SDL_Thread *ticker;
    if (!ticker) {
        ticker = SDL_CreateThread(poll_ticker, "poll ticker", NULL);
        if (!ticker) rt_fatal(NULL, "cannot start poll ticker: %s", SDL_GetError());
        SDL_DetachThread(ticker);
    }
}

void rt_poll(CPU *cpu) {
    rt_poll_pending = 0;
    if (rt_state_request) rt_state_poll(cpu);
    park_on_quit();
    sched_update();
    /* Like hardware, re-enter the handler while interrupts remain pending: the SDK handler services one
     * source per entry (the lowest bit), so a frequent source such as HBlank must not starve the rest. */
    for (int i = 0; i < 32 && (io_get32(0x208) & 1) && irq_wanted() && !(cpu->cpsr_ctl & 0x80); i++) {
        rt_enter_irq(cpu);
    }
}

/* Cycles until the next scanline boundary or timer overflow. */
static uint64_t next_event_delay(void) {
    uint64_t now = sched_now();
    uint64_t next = (now / RT_LINE_CYCLES + 1) * RT_LINE_CYCLES;
    for (int n = 0; n < 4; n++) {
        Timer *t = &timers[n];
        if (t->running && !t->countup && t->start + timer_period(t) < next) next = t->start + timer_period(t);
    }
    return next > now ? next - now : 0;
}

void rt_halt(CPU *cpu) {
    game_waiting = 1;
    for (;;) {
        park_on_quit();
        sched_update();
        if (irq_wanted()) break;
        uint64_t cycles = next_event_delay();
        SDL_DelayPrecise((Uint64)((double)cycles * (1e9 / RT_CLOCK_HZ) / speed) + 1);
    }
    game_waiting = 0;
    rt_poll(cpu);
}

void rt_intr_wait(CPU *cpu, int discard, uint32_t mask) {
    uint32_t check = rt_dtcm_base + 0x3FF8u; /* BIOS IRQ check flags, set by the game's IRQ handlers */
    io_set32(0x208, 1);
    if (discard) rt_w32(check, rt_r32(check) & ~mask);
    for (;;) {
        uint32_t flags = rt_r32(check);
        if (flags & mask) {
            rt_w32(check, flags & ~mask);
            return;
        }
        rt_halt(cpu);
    }
}

void sched_state(RtStateIO *s) {
    uint64_t now = sched_now();
    RT_STATE(s, now);
    RT_STATE(s, rt_frame_count);
    RT_STATE(s, irq_flags);
    RT_STATE(s, line_done);
    RT_STATE(s, line_phase);
    RT_STATE(s, timers);
    RT_STATE(s, dmas);
    if (rt_state_loading(s)) {
        /* Emulated time continues from the saved moment. */
        base_ns = SDL_GetTicksNS();
        base_cycles = now;
        rt_poll_pending = 1;
    }
}
