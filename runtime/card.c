/* Cartridge bus, KEY2 data mode as seen by the ARM9 (encryption is transparent here).
 * Transfers complete instantly: data is staged when ROMCTRL starts and drained through 0x04100010
 * by the CPU or by a DMA channel in "DS cartridge slot" mode. */
#include <stdio.h>
#include <stdlib.h>

#include "rt.h"

#define ROMCTRL 0x1A4u
#define CHIP_ID 0x00007FC2u /* must match the value boot.c stores at 0x027FF800 */

static uint8_t buf[0x4000];
static uint32_t len, pos;

void card_reset(void) { len = pos = 0; }

/* A card-mode DMA channel fetches each time a word becomes ready; keep triggering while it consumes data. */
void card_pump_dma(void) {
    for (uint32_t before = ~0u; pos < len && pos != before;) {
        before = pos;
        dma_trigger(DMA_START_CARD);
    }
}

static void finish(void) {
    io_set32(ROMCTRL, io_get32(ROMCTRL) & ~((1u << 31) | (1u << 23)));
    if (io_get16(0x1A0) & (1u << 14)) irq_raise(IRQ_CARD_DONE);
}

void card_romctrl_written(void) {
    uint32_t ctl = io_get32(ROMCTRL);
    uint32_t bs = (ctl >> 24) & 7;
    len = bs == 0 ? 0 : bs == 7 ? 4 : 0x100u << bs;
    pos = 0;
    uint8_t cmd[8];
    memcpy(cmd, &rt_io[0x1A8], 8);

    switch (cmd[0]) {
    case 0xB7: { /* read data */
        uint32_t addr = (uint32_t)cmd[1] << 24 | (uint32_t)cmd[2] << 16 | (uint32_t)cmd[3] << 8 | cmd[4];
        if (addr < 0x8000u) addr = 0x8000u + (addr & 0x1FFu); /* secure area is not readable in this mode */
        for (uint32_t i = 0; i < len; i++) {
            /* Reads wrap within a 4 KB page. */
            uint32_t a = (addr & ~0xFFFu) | ((addr + i) & 0xFFFu);
            buf[i] = a < rt_rom.size ? rt_rom.data[a] : rt_rom.more && a - rt_rom.more_at < rt_rom.more_size ? rt_rom.more[a - rt_rom.more_at] : 0xFF;
        }
        break;
    }
    case 0xB8: /* chip ID */
        for (uint32_t i = 0; i < len; i += 4) memcpy(&buf[i], &(uint32_t){CHIP_ID}, 4);
        break;
    default:
        rt_log("card: unhandled command %02x %02x%02x%02x%02x %02x%02x%02x", cmd[0], cmd[1], cmd[2], cmd[3], cmd[4],
               cmd[5], cmd[6], cmd[7]);
        memset(buf, 0xFF, len);
        break;
    }

    if (len == 0) {
        finish();
        return;
    }
    io_set32(ROMCTRL, ctl | (1u << 23));
    card_pump_dma();
}

/* ---- backup memory (save chip) ----
 * CARDiCommandArg (shared with the ARM7): +0 result, +4 backup type, +8 id, +0xC src, +0x10 dst, +0x14 len.
 * Backup type: bits 0-7 device (1 EEPROM, 2 FLASH, 3 FRAM), bits 8-15 log2(size). */
enum { REQ_IDENTIFY = 2, REQ_READ = 6, REQ_WRITE = 7, REQ_PROGRAM = 8, REQ_VERIFY = 9 };
enum { RESULT_SUCCESS = 0, RESULT_FAILURE = 1, RESULT_INVALID_PARAM = 2 };

static uint8_t *backup;
static uint32_t backup_size;

static void backup_open(uint32_t size) {
    backup = malloc(size);
    if (!backup) rt_fatal(rt_cur_cpu, "out of memory for backup");
    backup_size = size;
    memset(backup, 0xFF, size); /* erased chip */
    FILE *f = fopen(rt_rom.save_path, "rb");
    if (f) {
        size_t got = fread(backup, 1, size, f);
        fclose(f);
        rt_log("card: loaded %zu bytes of save data from %s", got, rt_rom.save_path);
    }
}

static void backup_flush(void) {
    FILE *f = fopen(rt_rom.save_path, "wb");
    if (!f || fwrite(backup, 1, backup_size, f) != backup_size) rt_log("card: cannot write %s", rt_rom.save_path);
    if (f) fclose(f);
}

uint32_t card_backup_request(uint32_t req, uint32_t cmd) {
    uint32_t type = rt_r32(cmd + 4);
    uint32_t src = rt_r32(cmd + 0xC), dst = rt_r32(cmd + 0x10), len = rt_r32(cmd + 0x14);
    uint32_t size = 1u << ((type >> 8) & 0xFF);
    if (req == REQ_IDENTIFY) return RESULT_SUCCESS;
    if (req < REQ_READ || req > REQ_VERIFY) {
        rt_log("card: backup request %u not emulated", req);
        return RESULT_SUCCESS;
    }
    if (!backup) backup_open(size);
    uint32_t off = req == REQ_READ ? src : dst;
    if (len > backup_size || off > backup_size - len) return RESULT_INVALID_PARAM;
    switch (req) {
    case REQ_READ:
        for (uint32_t i = 0; i < len; i++) rt_w8(dst + i, backup[off + i]);
        return RESULT_SUCCESS;
    case REQ_WRITE:
    case REQ_PROGRAM:
        for (uint32_t i = 0; i < len; i++) backup[off + i] = (uint8_t)rt_r8(src + i);
        backup_flush();
        return RESULT_SUCCESS;
    default: /* REQ_VERIFY */
        for (uint32_t i = 0; i < len; i++) {
            if (backup[off + i] != rt_r8(src + i)) return RESULT_FAILURE;
        }
        return RESULT_SUCCESS;
    }
}

uint32_t card_read_data(void) {
    if (pos >= len) return 0xFFFFFFFFu;
    uint32_t v;
    memcpy(&v, &buf[pos], 4);
    pos += 4;
    if (pos >= len) finish();
    return v;
}

/* The backup chip's contents are part of the state; the .sav file changes only when the game writes again. */
void card_state(RtStateIO *s) {
    static uint8_t image[0x10000];
    uint32_t size = backup ? backup_size : 0;
    if (size > sizeof image) rt_fatal(NULL, "state: backup chip larger than %u bytes", (unsigned)sizeof image);
    if (rt_state_saving(s) && size) memcpy(image, backup, size);
    RT_STATE(s, buf);
    RT_STATE(s, len);
    RT_STATE(s, pos);
    RT_STATE(s, size);
    RT_STATE(s, image);
    if (rt_state_loading(s) && size) {
        if (!backup) backup_open(size);
        if (size <= sizeof image && size == backup_size) memcpy(backup, image, size);
    }
}
