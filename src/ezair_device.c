#include "ezair_device.h"

#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/arm/arm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- EZ-FLASH Air register addresses (GBA cart space) ----
#define R_SD_CTRL   0x09400000u   // 1=data, 0=disable, 3=status
#define R_SD_LBA_LO 0x09600000u
#define R_SD_LBA_HI 0x09620000u
#define R_SD_COUNT  0x09640000u   // bit 0x8000 = write direction
#define R_PSRAM_PG  0x09860000u
#define R_ROM_PG    0x09880000u
#define R_RAM_PG    0x09C00000u
#define R_BUFCTRL   0x09420000u
#define R_SPI_CTRL  0x09660000u
#define R_SPI_WR    0x09680000u
#define R_RTC       0x096A0000u

#define NORWIN_BASE 0x09000000u   // 8 MB NOR window
#define NORWIN_END  0x09800000u
#define DATAWIN_BASE 0x09E00000u  // shared 512B data/status FIFO window
#define DATAWIN_END  0x09E80000u

#define NOR_DEVICE_ID 0x223Du
#define SD_BUSY       0xEEE1u

// ---- device state ----
static uint8_t* s_cart;            // 32 MB, == gba->memory.rom
static char     s_sd_path[1024];
static int      s_trace;

static uint32_t s_sd_lba;
static uint16_t s_sd_count;        // raw (may have 0x8000 write bit)
static int      s_sd_pending_read;
static int      s_sd_pending_write;

// NOR JEDEC unlock tracking (per command-cycle, state machine is simple)
static int      s_nor_unlock;      // count of valid unlock writes seen
static int      s_nor_id_mode;     // autoselect (device-id) mode active
static int      s_nor_erase_armed; // saw 0x80 (erase setup)
static int      s_nor_prog_armed;  // saw 0xA0 (program setup)

static struct ARMCore* s_cpu;
static void (*orig_store8)(struct ARMCore*, uint32_t, int8_t, int*);
static void (*orig_store16)(struct ARMCore*, uint32_t, int16_t, int*);
static void (*orig_store32)(struct ARMCore*, uint32_t, int32_t, int*);

static inline void cart_w16(uint32_t cart_off, uint16_t v) {
	s_cart[cart_off]     = (uint8_t)v;
	s_cart[cart_off + 1] = (uint8_t)(v >> 8);
}
static inline uint16_t cart_r16(uint32_t cart_off) {
	return (uint16_t)(s_cart[cart_off] | (s_cart[cart_off + 1] << 8));
}

uint16_t ezair_nor_read16(uint32_t nor_off) {
	return cart_r16(EZ_NORWIN_OFF + nor_off);
}

void ezair_set_trace(int on) { s_trace = on; }
#define TRACE(...) do { if (s_trace) { fprintf(stderr, "[ezair] " __VA_ARGS__); } } while (0)

// ---- SD image I/O ----
static void sd_read_into_window(void) {
	if (!s_sd_path[0]) { memset(s_cart + EZ_DATAWIN_OFF, 0, EZ_SD_SECTOR); return; }
	uint16_t blocks = s_sd_count & 0x7FFFu;
	if (blocks == 0 || blocks > 8) blocks = blocks ? 8 : 1;
	FILE* f = fopen(s_sd_path, "rb");
	if (!f) { TRACE("SD read: cannot open %s\n", s_sd_path); return; }
	if (fseek(f, (long)s_sd_lba * EZ_SD_SECTOR, SEEK_SET) == 0) {
		size_t n = fread(s_cart + EZ_DATAWIN_OFF, 1, (size_t)blocks * EZ_SD_SECTOR, f);
		TRACE("SD read  LBA=%u blocks=%u -> %zu bytes\n", s_sd_lba, blocks, n);
	}
	fclose(f);
}

static void sd_write_from_window(void) {
	if (!s_sd_path[0]) return;
	uint16_t blocks = s_sd_count & 0x7FFFu;
	if (blocks == 0 || blocks > 8) blocks = blocks ? 8 : 1;
	FILE* f = fopen(s_sd_path, "r+b");
	if (!f) { TRACE("SD write: cannot open %s\n", s_sd_path); return; }
	if (fseek(f, (long)s_sd_lba * EZ_SD_SECTOR, SEEK_SET) == 0) {
		fwrite(s_cart + EZ_DATAWIN_OFF, 1, (size_t)blocks * EZ_SD_SECTOR, f);
		TRACE("SD write LBA=%u blocks=%u\n", s_sd_lba, blocks);
	}
	fclose(f);
}

// ---- NOR JEDEC over the 0x09000000 window ----
// Reads come straight from the cart buffer (persisted). We only act on writes:
// unlock cycles 0xAA@0xAAA / 0x55@0x554, then command @0xAAA; sector erase
// (0x80..0x30), word program (0xA0,data), autoselect ID (0x90 -> read 0x223D
// at win+0x1C), reset (0xF0). Buffered program (0x25/0x29) and completion
// polling ("read twice until equal") are satisfied by writing instantly.
static void nor_write(uint32_t addr, uint16_t val) {
	uint32_t off = addr - NORWIN_BASE;        // window-relative byte offset
	uint32_t cart_off = EZ_NORWIN_OFF + off;

	if (s_nor_prog_armed) {                    // data cycle of a word program
		cart_w16(cart_off, val);
		s_nor_prog_armed = 0;
		TRACE("NOR program  win+0x%X = 0x%04X\n", off, val);
		return;
	}
	if (s_nor_erase_armed && val == 0x30) {    // sector-erase confirm
		uint32_t sec = cart_off & ~0x1FFFFu;   // 128 KB sector
		memset(s_cart + sec, 0xFF, 0x20000u);
		s_nor_erase_armed = 0;
		s_nor_unlock = 0;
		TRACE("NOR erase    sector @ win+0x%X\n", sec - EZ_NORWIN_OFF);
		return;
	}

	uint32_t wo = off & 0xFFFu;                // unlock addresses are window-relative
	if (wo == 0xAAAu && val == 0xAAu) { s_nor_unlock = 1; return; }
	if (wo == 0x554u && val == 0x55u && s_nor_unlock == 1) { s_nor_unlock = 2; return; }
	if (wo == 0xAAAu && s_nor_unlock == 2) {
		switch (val) {
			case 0x80: s_nor_erase_armed = 1; s_nor_unlock = 0; return; // erase setup
			case 0xA0: s_nor_prog_armed = 1;  s_nor_unlock = 0; return; // program setup
			case 0x90: // autoselect: expose device id at win+0x1C
				s_nor_id_mode = 1;
				cart_w16(EZ_NORWIN_OFF + 0x1Cu, NOR_DEVICE_ID);
				s_nor_unlock = 0;
				TRACE("NOR autoselect (id=0x%04X)\n", NOR_DEVICE_ID);
				return;
			default: s_nor_unlock = 0; return;
		}
	}
	if (val == 0xF0) {                         // reset out of id/cmd mode
		if (s_nor_id_mode) { cart_w16(EZ_NORWIN_OFF + 0x1Cu, 0xFFFFu); s_nor_id_mode = 0; }
		s_nor_unlock = s_nor_erase_armed = s_nor_prog_armed = 0;
		return;
	}
	// erase 0xAA/0x55 second-unlock before 0x30 keeps s_nor_unlock churning; ignore.
	s_nor_unlock = 0;
}

// returns 1 if the write was consumed by the device (swallow it)
static int ezair_write(uint32_t addr, uint16_t val, int width) {
	switch (addr) {
		case R_SD_LBA_LO: s_sd_lba = (s_sd_lba & 0xFFFF0000u) | val; return 1;
		case R_SD_LBA_HI: s_sd_lba = (s_sd_lba & 0x0000FFFFu) | ((uint32_t)val << 16); return 1;
		case R_SD_COUNT:
			s_sd_count = val;
			if (val & 0x8000u) s_sd_pending_write = 1; else s_sd_pending_read = 1;
			return 1;
		case R_SD_CTRL:
			if (val == 3) {                    // status/read-state: stage data now
				if (s_sd_pending_read) { sd_read_into_window(); s_sd_pending_read = 0; }
				else if (!s_sd_pending_write) cart_w16(EZ_DATAWIN_OFF, 0x0000); // ready
			} else if (val == 1) {
				if (s_sd_pending_write) { sd_write_from_window(); s_sd_pending_write = 0; }
			}
			return 1;
		case R_SPI_CTRL:
			if (val == 1) cart_w16(EZ_DATAWIN_OFF, 0xC003); // FPGA ver V03, high nibble 0xC
			return 1;
		case R_SPI_WR: return 1;
		case R_ROM_PG:  TRACE("ROM page  = 0x%04X\n", val); return 1;
		case R_PSRAM_PG: case R_RAM_PG: case R_BUFCTRL: case R_RTC: return 1;
		default: break;
	}
	if (addr >= NORWIN_BASE && addr < NORWIN_END) { nor_write(addr, val); return 1; }
	if (addr >= DATAWIN_BASE && addr < DATAWIN_END) {
		// SD write payload DMA'd in by the guest: capture into our window copy.
		uint32_t off = EZ_DATAWIN_OFF + (addr - DATAWIN_BASE);
		if (width == 4) { cart_w16(off, val & 0xFFFF); cart_w16(off + 2, (val >> 16) & 0xFFFF); }
		else cart_w16(off, val);
		return 1;
	}
	// 0x08xxxxxx knock writes and 0x09FC/0x09FE commit writes: harmless, swallow.
	if ((addr & 0xFF000000u) == 0x08000000u) return 1;
	if (addr == 0x09FC0000u || addr == 0x09FE0000u) return 1;
	return 0;
}

static void hook_store16(struct ARMCore* cpu, uint32_t addr, int16_t value, int* cyc) {
	if (addr >= 0x08000000u && addr <= 0x09FFFFFFu && ezair_write(addr, (uint16_t)value, 2)) return;
	orig_store16(cpu, addr, value, cyc);
}
static void hook_store32(struct ARMCore* cpu, uint32_t addr, int32_t value, int* cyc) {
	if (addr >= 0x08000000u && addr <= 0x09FFFFFFu && ezair_write(addr, (uint32_t)value, 4)) return;
	orig_store32(cpu, addr, value, cyc);
}
static void hook_store8(struct ARMCore* cpu, uint32_t addr, int8_t value, int* cyc) {
	if (addr >= 0x08000000u && addr <= 0x09FFFFFFu && ezair_write(addr, (uint8_t)value, 1)) return;
	orig_store8(cpu, addr, value, cyc);
}

int ezair_attach(struct mCore* core, uint8_t* cart, const char* sd_path) {
	struct GBA* gba = (struct GBA*) core->board;
	s_cpu = gba->cpu;
	s_cart = cart;
	s_sd_path[0] = 0;
	if (sd_path) { strncpy(s_sd_path, sd_path, sizeof(s_sd_path) - 1); }

	orig_store8  = s_cpu->memory.store8;
	orig_store16 = s_cpu->memory.store16;
	orig_store32 = s_cpu->memory.store32;
	s_cpu->memory.store8  = hook_store8;
	s_cpu->memory.store16 = hook_store16;
	s_cpu->memory.store32 = hook_store32;
	return 0;
}
