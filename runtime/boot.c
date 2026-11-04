/* Loads the ROM and recreates the state the DS firmware leaves before jumping to the ARM9 entry. */
#include <stdio.h>
#include <stdlib.h>

#include "cheevos.h"
#include "dispatch.h"
#include "display.h"
#include "rt.h"
#include "video.h"
#include "widescreen.h"

RtRom rt_rom;

static uint32_t rom32(uint32_t off) {
    uint32_t v;
    memcpy(&v, rt_rom.data + off, 4);
    return v;
}

int rt_load_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0x200) { fclose(f); return 0; }
    rt_rom.data = malloc((size_t)size);
    rt_rom.size = (size_t)size;
    size_t got = rt_rom.data ? fread(rt_rom.data, 1, (size_t)size, f) : 0;
    fclose(f);
    if (got != (size_t)size) return 0;
    snprintf(rt_rom.path, sizeof rt_rom.path, "%s", path);
    size_t n = strlen(path);
    const char *dot = strrchr(path, '.');
    if (dot && strpbrk(dot, "/\\") == NULL) n = (size_t)(dot - path);
    snprintf(rt_rom.save_path, sizeof rt_rom.save_path, "%.*s.sav", (int)n, path);
    snprintf(rt_rom.state_path, sizeof rt_rom.state_path, "%.*s.state", (int)n, path);
    if (memcmp(rt_rom.data + 0x0C, "YQUE", 4) != 0) {
        rt_log("warning: game code is not YQUE (Chrono Trigger USA); generated code will not match");
    }
    return 1;
}

/* Firmware user settings (GBATEK "DS Firmware User Settings"), as the firmware copies them to RAM. */
#define USER_SETTINGS 0x027FFC80u

static void write_user_settings(uint32_t a) {
    for (uint32_t i = 0; i < 0x70; i++) rt_w8(a + i, 0);
    rt_w16(a + 0x00, 5);    /* version */
    rt_w8(a + 0x03, 1);     /* birthday month */
    rt_w8(a + 0x04, 1);     /* birthday day */
    static const char name[] = "Crono";
    for (uint32_t i = 0; i < sizeof name - 1; i++) rt_w16(a + 0x06 + 2 * i, (uint8_t)name[i]);
    rt_w16(a + 0x1A, sizeof name - 1);
    /* Touch calibration: two ADC/screen point pairs, 16 ADC units per pixel. */
    rt_w16(a + 0x58, 32 * 16); rt_w16(a + 0x5A, 32 * 16); rt_w8(a + 0x5C, 32); rt_w8(a + 0x5D, 32);
    rt_w16(a + 0x5E, 224 * 16); rt_w16(a + 0x60, 160 * 16); rt_w8(a + 0x62, 224); rt_w8(a + 0x63, 160);
    /* Language (1 English, 2 French: the two the USA game has), backlight max. */
    rt_w16(a + 0x64, (display_setting_get(SET_LANGUAGE) == LANGUAGE_FRENCH ? 2 : 1) | 3 << 4);
}

int rt_french(void) { return (rt_r16(USER_SETTINGS + 0x64) & 7) == 2; }

/* Console Wi-Fi MAC address, which the ARM7 copies from firmware to RAM at power-on (OS_GetMacAddress).
 * DS Protect's emulator check (func_0204e334) treats an all-zero MAC, or the one emulators used
 * (00:09:BF:00:00:31), as an emulator; the game then routes the first time warp into a copy that never
 * ends. Any other Nintendo-OUI address is what a retail DS has. */
#define CONSOLE_MAC 0x027FFCF4u

void rt_write_console_mac(void) {
    static const uint8_t mac[6] = {0x00, 0x09, 0xBF, 0x45, 0x47, 0x47};
    for (uint32_t i = 0; i < 6; i++) rt_w8(CONSOLE_MAC + i, mac[i]);
}

int rt_console_mac_missing(void) {
    for (uint32_t i = 0; i < 6; i++) {
        if (rt_r8(CONSOLE_MAC + i)) return 0;
    }
    return 1;
}

void rt_boot(CPU *cpu) {
    uint32_t arm9_off = rom32(0x20), arm9_entry = rom32(0x24), arm9_addr = rom32(0x28), arm9_size = rom32(0x2C);
    if ((uint64_t)arm9_off + arm9_size > rt_rom.size) rt_fatal(cpu, "ARM9 binary outside ROM");
    cheevos_reset(); /* powered on again: achievements watch the new run */

    /* The firmware copies the cartridge header to 0x027FFE00 and the ARM9 binary (still compressed) to RAM. */
    for (uint32_t i = 0; i < 0x170; i++) rt_w8(0x027FFE00u + i, rt_rom.data[i]);
    for (uint32_t i = 0; i < arm9_size; i++) rt_w8(arm9_addr + i, rt_rom.data[arm9_off + i]);
    /* Cartridge chip ID as the firmware stores it (Macronix-style 128 MB mask ROM). */
    uint32_t chip_id = 0x00007FC2u;
    rt_w32(0x027FF800u, chip_id);
    rt_w32(0x027FF804u, chip_id);
    rt_w32(0x027FFC00u, chip_id);
    rt_w32(0x027FFC04u, chip_id);
    rt_w16(0x027FFC40u, 1); /* boot indicator: booted from cartridge */
    write_user_settings(USER_SETTINGS);
    rt_write_console_mac();

    if (arm9_entry != rt_game->entry_point) {
        rt_fatal(cpu, "ROM entry 0x%08x does not match recompiled entry 0x%08x", arm9_entry, rt_game->entry_point);
    }
    hw_reset();
    video_reset();
    ws_reset();
    pxi_reset();
    card_reset();
    sched_init();
    rt_cpu_reset(cpu);
    R[13] = 0x02FFFC80u; /* temporary stack; crt0 sets its own */
    rt_log("boot: ARM9 0x%x bytes at 0x%08x, entry 0x%08x", arm9_size, arm9_addr, arm9_entry);
    rt_ret_push(cpu, 0);
    if (getenv("EGG_SELFTEST")) rt_selftest(cpu);
    if (getenv("EGG_WATCH_WRITE")) rt_watch_word = (uint32_t)strtoul(getenv("EGG_WATCH_WRITE"), NULL, 16) & ~3u;
    RT_CALL_ADDR(arm9_entry, 0);
    rt_fatal(cpu, "ARM9 entry function returned");
}
