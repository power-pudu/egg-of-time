/* ARM9 memory map. Main RAM and DTCM fast paths are inline in recomp.h; everything else lands here. */
#include <stdio.h>
#include "rt.h"
#include "video.h"

uint8_t rt_main_ram[RT_MAIN_RAM_SIZE];
uint8_t rt_dtcm[RT_DTCM_SIZE];
uint32_t rt_dtcm_base = 0x027E0000u;
uint8_t rt_itcm[RT_ITCM_SIZE];
uint8_t rt_shared_wram[RT_SHARED_WRAM_SIZE];
uint8_t rt_palette[RT_PALETTE_SIZE];
uint8_t rt_oam[RT_OAM_SIZE];
uint8_t rt_vram[RT_VRAM_SIZE];
uint8_t rt_io[RT_IO_SIZE];
uint32_t rt_watch_word = ~0u;

void rt_watch_hit(uint32_t a, uint32_t v, int size) {
    CPU *cpu = rt_cur_cpu;
    fprintf(stderr, "[watch] w%d %08x = %x  lr=%08x calls:", size * 8, a, v, cpu ? cpu->r[14] : 0);
    for (uint32_t i = cpu ? cpu->ret_depth : 0, n = 0; i > 0 && n < 8; i--, n++) fprintf(stderr, " %08x", cpu->ret_stack[i - 1]);
    fprintf(stderr, "\n");
}

/* Host pointer for a plain-memory address, or NULL for I/O and unmapped space. */
static uint8_t *host_ptr(uint32_t a) {
    if (a - rt_dtcm_base < RT_DTCM_SIZE) return &rt_dtcm[a - rt_dtcm_base];
    switch (a >> 24) {
    case 0x00:
    case 0x01: return &rt_itcm[a & (RT_ITCM_SIZE - 1)];
    case 0x02: return &rt_main_ram[a & (RT_MAIN_RAM_SIZE - 1)];
    case 0x03: return &rt_shared_wram[a & (RT_SHARED_WRAM_SIZE - 1)];
    case 0x05: return &rt_palette[a & (RT_PALETTE_SIZE - 1)];
    case 0x06: return video_vram_ptr(a);
    case 0x07: return &rt_oam[a & (RT_OAM_SIZE - 1)];
    default: return NULL;
    }
}

/* Unmapped regions (GBA slot, BIOS) read as zero; writes are dropped. */
static uint32_t io_read(uint32_t a, int size) {
    uint32_t sh = (a & 3) * 8;
    uint32_t w = hw_read32(a & ~3u) >> sh;
    return size == 4 ? w : w & ((1u << (size * 8)) - 1);
}

static void io_write(uint32_t a, uint32_t v, int size) {
    uint32_t sh = (a & 3) * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1) << sh;
    hw_write32(a & ~3u, v << sh, mask);
}

uint32_t rt_r32_slow(uint32_t a) {
    uint8_t *p = host_ptr(a);
    if (p) { uint32_t v; memcpy(&v, p, 4); return v; }
    if ((a >> 24) == 0x04) return io_read(a, 4);
    return 0;
}
uint32_t rt_r16_slow(uint32_t a) {
    uint8_t *p = host_ptr(a);
    if (p) { uint16_t v; memcpy(&v, p, 2); return v; }
    if ((a >> 24) == 0x04) return io_read(a, 2);
    return 0;
}
uint32_t rt_r8_slow(uint32_t a) {
    uint8_t *p = host_ptr(a);
    if (p) return *p;
    if ((a >> 24) == 0x04) return io_read(a, 1);
    return 0;
}
void rt_w32_slow(uint32_t a, uint32_t v) {
    uint8_t *p = host_ptr(a);
    if (p) { memcpy(p, &v, 4); return; }
    if ((a >> 24) == 0x04) io_write(a, v, 4);
}
void rt_w16_slow(uint32_t a, uint32_t v) {
    uint8_t *p = host_ptr(a);
    if (p) { uint16_t h = (uint16_t)v; memcpy(p, &h, 2); return; }
    if ((a >> 24) == 0x04) io_write(a, v, 2);
}
void rt_w8_slow(uint32_t a, uint32_t v) {
    uint32_t region = a >> 24;
    if (region >= 0x05 && region <= 0x07) return; /* the ARM9 ignores 8-bit writes to palette, VRAM and OAM */
    uint8_t *p = host_ptr(a);
    if (p) { *p = (uint8_t)v; return; }
    if (region == 0x04) io_write(a, v, 1);
}

void memory_power_on(void) {
    memset(rt_main_ram, 0, sizeof rt_main_ram);
    memset(rt_dtcm, 0, sizeof rt_dtcm);
    rt_dtcm_base = 0x027E0000u;
    memset(rt_itcm, 0, sizeof rt_itcm);
    memset(rt_shared_wram, 0, sizeof rt_shared_wram);
    memset(rt_palette, 0, sizeof rt_palette);
    memset(rt_oam, 0, sizeof rt_oam);
    memset(rt_vram, 0, sizeof rt_vram);
    memset(rt_io, 0, sizeof rt_io);
}

void memory_state(RtStateIO *s) {
    RT_STATE(s, rt_main_ram);
    RT_STATE(s, rt_dtcm);
    RT_STATE(s, rt_dtcm_base);
    RT_STATE(s, rt_itcm);
    RT_STATE(s, rt_shared_wram);
    RT_STATE(s, rt_palette);
    RT_STATE(s, rt_oam);
    RT_STATE(s, rt_vram);
    RT_STATE(s, rt_io);
    /* States saved before the runtime provided a MAC address would keep failing DS Protect. */
    if (rt_state_loading(s) && rt_console_mac_missing()) rt_write_console_mac();
}
