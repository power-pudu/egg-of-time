/* Runtime-internal declarations (not used by generated code). */
#pragma once

#include <stddef.h>
#include "recomp.h"

#ifdef __GNUC__
#define RT_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define RT_PRINTF(f, a)
#endif

_Noreturn void rt_fatal(CPU *cpu, const char *fmt, ...) RT_PRINTF(2, 3);
void rt_log(const char *fmt, ...) RT_PRINTF(1, 2);

/* ---- memory.c ---- */
#define RT_SHARED_WRAM_SIZE 0x8000u
#define RT_PALETTE_SIZE 0x800u
#define RT_OAM_SIZE 0x800u
#define RT_VRAM_SIZE 0xA4000u
#define RT_IO_SIZE 0x2000u
extern uint8_t rt_itcm[RT_ITCM_SIZE];
extern uint8_t rt_shared_wram[RT_SHARED_WRAM_SIZE];
extern uint8_t rt_palette[RT_PALETTE_SIZE];
extern uint8_t rt_oam[RT_OAM_SIZE];
extern uint8_t rt_vram[RT_VRAM_SIZE];
extern uint8_t rt_io[RT_IO_SIZE]; /* backing store for 0x04000000-0x04001FFF */

static inline uint32_t io_get32(uint32_t off) { uint32_t v; memcpy(&v, &rt_io[off], 4); return v; }
static inline uint16_t io_get16(uint32_t off) { uint16_t v; memcpy(&v, &rt_io[off], 2); return v; }
static inline void io_set32(uint32_t off, uint32_t v) { memcpy(&rt_io[off], &v, 4); }
static inline void io_set16(uint32_t off, uint16_t v) { memcpy(&rt_io[off], &v, 2); }

/* ---- hw.c: I/O register dispatch (aligned 32-bit words; mask = bytes being written) ---- */
uint32_t hw_read32(uint32_t addr);
void hw_write32(uint32_t addr, uint32_t value, uint32_t mask);
void hw_reset(void);

/* ---- sched.c: emulated time, interrupts, timers, DMA ---- */
#define RT_CLOCK_HZ 33513982u   /* ARM9 bus clock; timers and video timing run on it */
#define RT_LINE_CYCLES 2130u
#define RT_HDRAW_CYCLES 1536u
#define RT_FRAME_LINES 263u
enum {
    IRQ_VBLANK = 1u << 0, IRQ_HBLANK = 1u << 1, IRQ_VCOUNT = 1u << 2, IRQ_TIMER0 = 1u << 3,
    IRQ_DMA0 = 1u << 8, IRQ_KEYPAD = 1u << 12, IRQ_IPC_SYNC = 1u << 16, IRQ_IPC_SEND_EMPTY = 1u << 17,
    IRQ_IPC_RECV = 1u << 18, IRQ_CARD_DONE = 1u << 19, IRQ_GXFIFO = 1u << 21,
};
enum { DMA_START_IMMEDIATE, DMA_START_VBLANK, DMA_START_HBLANK, DMA_START_DISPLAY, DMA_START_MAINMEM,
       DMA_START_CARD, DMA_START_GBA, DMA_START_GXFIFO };
void sched_init(void);
uint64_t sched_now(void); /* emulated cycles since boot */
void sched_update(void);  /* raise every event up to now */
void irq_raise(uint32_t bits);
uint32_t irq_if(void);
void irq_ack(uint32_t bits);
void irq_changed(void); /* IME/IE/IF or CPSR.I changed: deliver at the next poll */
uint16_t video_vcount(void);
uint16_t video_dispstat(void);
uint16_t timer_counter(int n);
void timer_control_written(int n, uint16_t old_ctl);
void dma_control_written(int ch, uint32_t old_cnt);
void dma_trigger(int start_mode);
void rt_halt(CPU *cpu);                          /* wait until IE & IF != 0, then deliver */
void rt_intr_wait(CPU *cpu, int discard, uint32_t mask);
int rt_stop_game(uint32_t timeout_ms); /* host: park the game thread at its next poll; 0 if it did not get there */
extern uint64_t rt_frame_count;                  /* VBlanks since boot */

/* ---- pxi.c: IPC with the (high-level emulated) ARM7 ---- */
void pxi_reset(void);
uint16_t pxi_sync_read(void);
void pxi_sync_write(uint16_t v);
uint16_t pxi_fifocnt_read(void);
void pxi_fifocnt_write(uint16_t v);
void pxi_send(uint32_t v);
uint32_t pxi_recv(void);
void pxi_update(uint64_t now); /* ARM7-side timed events (sound driver frames, alarms) */
void pxi_arm7_send(uint32_t v); /* queue an ARM7 -> ARM9 FIFO word */

/* ---- card.c: cartridge bus ---- */
void card_reset(void);
void card_romctrl_written(void);
uint32_t card_read_data(void);
void card_pump_dma(void);
/* Backup memory (save chip) request from the CARD library through the ARM7; returns a CARD_RESULT. */
uint32_t card_backup_request(uint32_t req, uint32_t cmd);

/* ---- cpu.c ---- */
void rt_cpu_reset(CPU *cpu);
uint32_t rt_get_cpsr(CPU *cpu);
void rt_enter_irq(CPU *cpu);              /* BIOS IRQ vector: deliver one interrupt now */
void rt_switch_mode(CPU *cpu, uint32_t mode);

/* ---- input (written by the host thread) ---- */
enum { KEY_A = 1 << 0, KEY_B = 1 << 1, KEY_SELECT = 1 << 2, KEY_START = 1 << 3, KEY_RIGHT = 1 << 4,
       KEY_LEFT = 1 << 5, KEY_UP = 1 << 6, KEY_DOWN = 1 << 7, KEY_R = 1 << 8, KEY_L = 1 << 9,
       KEY_X = 1 << 10, KEY_Y = 1 << 11 };
extern volatile uint32_t rt_keys;          /* pressed keys */
extern volatile int rt_touch_down, rt_touch_x, rt_touch_y; /* bottom-screen pixel coordinates */

/* ---- thread.c: game threads as coroutines ---- */
extern CPU *rt_cur_cpu; /* CPU state of the running game thread */
void rt_threads_run(void (*boot)(CPU *cpu)); /* runs the game; returns only when it stops */
void rt_threads_log(void);                     /* racy snapshot of every thread's return stack */
/* Calls a game function from runtime C code with arguments in r0-r2; returns r0. */
uint32_t rt_call_game(CPU *cpu, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2);

/* ---- boot.c ---- */
typedef struct RtRom {
    uint8_t *data;
    size_t size;
    char path[1024];      /* the ROM file */
    char save_path[1024]; /* cartridge backup memory file: ROM path with .sav */
    char state_path[1024]; /* save state (F5/F7): ROM path with .state */
} RtRom;
extern RtRom rt_rom;
int rt_load_rom(const char *path);
void rt_boot(CPU *cpu);
void rt_write_console_mac(void);  /* firmware data the ARM7 puts in RAM at power-on */
int rt_console_mac_missing(void);
void rt_selftest(CPU *cpu); /* selftest.c */

/* ---- state.c: save states ---- */
enum { RT_STATE_SAVE = 1, RT_STATE_LOAD = 2 };
extern volatile int rt_state_request; /* set by the host thread; handled at the game's next poll */
extern char rt_state_load_path[1024];  /* file for RT_STATE_LOAD (default: rt_rom.state_path) */
void rt_state_poll(CPU *cpu);
typedef struct RtStateIO RtStateIO;
/* Writes, checks or reads one tagged block; modules call it for each piece of their state. */
void rt_state_bytes(RtStateIO *s, const char *tag, void *p, size_t n);
#define RT_STATE(s, var) rt_state_bytes((s), #var, &(var), sizeof(var))
/* A block that states saved by older builds lack (after every other block); 1 if it was saved / loaded. */
int rt_state_optional(RtStateIO *s, const char *tag, void *p, size_t n);
int rt_state_loading(const RtStateIO *s); /* state is being applied (run post-load fixups) */
int rt_state_saving(const RtStateIO *s);
void memory_state(RtStateIO *s);
void cpu_state(RtStateIO *s);
void sched_state(RtStateIO *s);
void video_state(RtStateIO *s);
void pxi_state(RtStateIO *s);
void card_state(RtStateIO *s);
void snd_state(RtStateIO *s);
void spu_state(RtStateIO *s);
void threads_state(RtStateIO *s);
int rt_threads_can_save(void);            /* every thread is at a point it can be resumed from */
_Noreturn void rt_threads_restart(void);  /* game thread: drop all threads, load the requested state */
void rt_state_apply_requested(void);      /* scheduler: apply the state rt_state_poll checked */
