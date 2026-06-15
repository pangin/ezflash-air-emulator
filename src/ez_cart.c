// EZ-FLASH Air cartridge hardware model — pure implementation (no GBA core deps).
//
// See ez_cart.h for the role of this module. Everything here is plain C; the host
// adapter routes the core's memory accesses through ez_cart_write / ez_cart_map.
//
// Memory model:
//   * A dedicated NOR backing array (`nor`) holds the cartridge's flash linearly,
//     so games written at high NOR offsets (paged through the 8 MB window) land
//     where they belong and can be banked in at 0x08000000 in GAME mode.
//   * The 32 MB `cart` buffer (the core's ROM) holds the kernel image; its
//     0x09E00000 data window is pre-poked for SD/SPI results and read directly.
//
// What is modeled: SD block read/write (against a FAT image file), NOR JEDEC
// erase/word-program/autoselect-ID, the SPI FPGA-version read, ROM page/mode
// tracking, and OS/GAME bank switching (ez_cart_map).

#include "ez_cart.h"

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

#define NORWIN_BASE  0x09000000u  // 8 MB NOR window
#define NORWIN_SIZE  0x00800000u  // window size paged by R_ROM_PG (0x1000 per window)
#define NORWIN_END   (NORWIN_BASE + NORWIN_SIZE)
#define DATAWIN_BASE 0x09E00000u  // shared 512B data/status FIFO window
#define DATAWIN_END  0x09E80000u

#define NOR_DEVICE_ID 0x223Du
#define SD_BUSY       0xEEE1u

// ROM page/mode: SetRompage_MODE writes (page + MODE) where MODE bit = val & 1
// (SYSTEM_MODE_OS = 1 maps the kernel; SYSTEM_MODE_GAME = 0 maps a NOR game).
// In OS mode the page advances by 0x1000 per 8 MB NOR window; bit 0x8000 selects
// flash1 (the upper half of the 1 Gbit NOR). In GAME mode the page IS the game's
// rompage = (NOR byte offset >> 17), so the game starts at (page << 17).
#define ROM_MODE_OS   1
#define EZ_NOR_SIZE   0x08000000u  // 128 MB (1 Gbit) linear NOR backing
#define FLASH1_BASE   0x04000000u  // bit-0x8000 pages address the upper 64 MB

// In GAME mode the kernel is no longer mapped at 0x08000000. An empty NOR bank is
// modeled with a small "fill page" of the Thumb self-branch `B .` (0xE7FE): any
// fetch into the cart window mirrors it and the CPU spins in place — the runaway a
// real cart shows when code returns to ROM in the wrong (empty) bank. The kernel
// is built -mthumb, so 0xE7FE is the correct half-word to repeat.
#define EZ_FILL_SIZE  0x1000u
#define THUMB_SELF_BRANCH 0xE7FEu

// write-to-buffer (64-word) program phases (games are written this way)
#define NOR_BUF_IDLE    0
#define NOR_BUF_COUNT   1   // expecting the word-count-1 cycle
#define NOR_BUF_DATA    2   // streaming data words
#define NOR_BUF_CONFIRM 3   // expecting the 0x29 program-buffer-to-flash cycle

struct EzCart {
	uint8_t* cart;            // caller-owned 32 MB buffer (kernel ROM + data window)
	uint8_t* nor;             // owned linear NOR backing (EZ_NOR_SIZE)
	uint32_t nor_size;
	char     sd_path[1024];
	int      trace;

	// SD transfer state
	uint32_t sd_lba;
	uint16_t sd_count;        // raw (may carry the 0x8000 write bit)
	int      sd_pending_read;
	int      sd_pending_write;

	// NOR JEDEC command-cycle state machine
	int      nor_unlock;      // count of valid unlock writes seen
	int      nor_id_mode;     // autoselect (device-id) mode active
	int      nor_erase_armed; // saw 0x80 (erase setup)
	int      nor_prog_armed;  // saw 0xA0 (single-word program setup)
	int      nor_buf_state;   // write-to-buffer (64-word) program: 0=idle,1=count,2=data,3=confirm
	uint32_t nor_buf_remaining; // data words still expected in the current buffer

	// ROM page/mode (banking)
	int            mode_os;   // 1 = OS (kernel) mapped, 0 = GAME mapped
	uint16_t       rom_page;  // page register value with the mode bit stripped
	const uint8_t* fill_page; // mapped at 0x08000000 over an empty bank in GAME mode
	uint32_t       fill_size;
};

#define TRACE(...) do { if (c->trace) { fprintf(stderr, "[ezair] " __VA_ARGS__); } } while (0)

// ---- data-window helpers (live in the core's cart buffer, read directly) ----
static inline void cart_w16(EzCart* c, uint32_t cart_off, uint16_t v) {
	c->cart[cart_off]     = (uint8_t) v;
	c->cart[cart_off + 1] = (uint8_t) (v >> 8);
}

// ---- NOR backing helpers (linear flash, bounds-checked) ----
static inline void nor_w16(EzCart* c, uint32_t off, uint16_t v) {
	if (off + 1u < c->nor_size) { c->nor[off] = (uint8_t) v; c->nor[off + 1] = (uint8_t) (v >> 8); }
}
static inline uint16_t nor_r16(EzCart* c, uint32_t off) {
	if (off + 1u < c->nor_size) return (uint16_t) (c->nor[off] | (c->nor[off + 1] << 8));
	return 0xFFFF;
}

// Linear NOR base of the current 8 MB window in OS mode: 0x1000 of page == 8 MB,
// and bit 0x8000 selects the upper-64 MB flash1 half.
static inline uint32_t os_nor_base(uint16_t page) {
	uint32_t base = (uint32_t) ((page & 0x7FFFu) >> 12) * NORWIN_SIZE;
	if (page & 0x8000u) base += FLASH1_BASE;
	return base;
}

// ---- SD image I/O ----
static void sd_read_into_window(EzCart* c) {
	if (!c->sd_path[0]) { memset(c->cart + EZ_DATAWIN_OFF, 0, EZ_SD_SECTOR); return; }
	uint16_t blocks = c->sd_count & 0x7FFFu;
	if (blocks == 0 || blocks > 8) blocks = blocks ? 8 : 1;
	FILE* f = fopen(c->sd_path, "rb");
	if (!f) { TRACE("SD read: cannot open %s\n", c->sd_path); return; }
	if (fseek(f, (long) c->sd_lba * EZ_SD_SECTOR, SEEK_SET) == 0) {
		size_t n = fread(c->cart + EZ_DATAWIN_OFF, 1, (size_t) blocks * EZ_SD_SECTOR, f);
		TRACE("SD read  LBA=%u blocks=%u -> %zu bytes\n", c->sd_lba, blocks, n);
	}
	fclose(f);
}

static void sd_write_from_window(EzCart* c) {
	if (!c->sd_path[0]) return;
	uint16_t blocks = c->sd_count & 0x7FFFu;
	if (blocks == 0 || blocks > 8) blocks = blocks ? 8 : 1;
	FILE* f = fopen(c->sd_path, "r+b");
	if (!f) { TRACE("SD write: cannot open %s\n", c->sd_path); return; }
	if (fseek(f, (long) c->sd_lba * EZ_SD_SECTOR, SEEK_SET) == 0) {
		fwrite(c->cart + EZ_DATAWIN_OFF, 1, (size_t) blocks * EZ_SD_SECTOR, f);
		TRACE("SD write LBA=%u blocks=%u\n", c->sd_lba, blocks);
	}
	fclose(f);
}

// ---- NOR JEDEC over the 0x09000000 window ----
// Writes go to the linear NOR backing at the absolute offset selected by the
// current OS page. Unlock cycles 0xAA@0xAAA / 0x55@0x554, then command @0xAAA;
// sector erase (0x80..0x30), word program (0xA0,data), autoselect ID (0x90 ->
// read 0x223D at win+0x1C), reset (0xF0). Buffered program (0x25/0x29) and
// completion polling ("read twice until equal") are satisfied instantly.
static void nor_write(EzCart* c, uint32_t addr, uint16_t val) {
	uint32_t win_off = addr - NORWIN_BASE;     // 0..0x7FFFFF within the 8 MB window
	uint32_t base = os_nor_base(c->rom_page);  // linear NOR base of this window
	uint32_t abs = base + win_off;

	// Write-to-buffer (64-word) program: 0x25 @addr, count-1, N data words at
	// consecutive addresses, 0x29 @addr. This is how games are written.
	if (c->nor_buf_state == NOR_BUF_COUNT) {    // word-count-1 cycle (we count writes)
		c->nor_buf_remaining = (uint32_t) val + 1u;
		c->nor_buf_state = NOR_BUF_DATA;
		return;
	}
	if (c->nor_buf_state == NOR_BUF_DATA) {     // a buffered data word at its address
		nor_w16(c, abs, val);
		if (c->nor_buf_remaining == 0 || --c->nor_buf_remaining == 0) c->nor_buf_state = NOR_BUF_CONFIRM;
		return;
	}
	if (c->nor_buf_state == NOR_BUF_CONFIRM) {  // 0x29 program-buffer-to-flash
		c->nor_buf_state = NOR_BUF_IDLE;
		return;
	}

	if (c->nor_prog_armed) {                    // data cycle of a single-word program
		nor_w16(c, abs, val);
		c->nor_prog_armed = 0;
		TRACE("NOR program  nor+0x%X = 0x%04X\n", abs, val);
		return;
	}
	if (c->nor_erase_armed && val == 0x30) {    // sector-erase confirm
		uint32_t sec = abs & ~0x1FFFFu;         // 128 KB sector
		if (sec + 0x20000u <= c->nor_size) memset(c->nor + sec, 0xFF, 0x20000u);
		c->nor_erase_armed = 0;
		c->nor_unlock = 0;
		TRACE("NOR erase    sector @ nor+0x%X\n", sec);
		return;
	}

	uint32_t wo = win_off & 0xFFFu;             // unlock addresses are window-relative
	if (wo == 0xAAAu && val == 0xAAu) { c->nor_unlock = 1; return; }
	if (wo == 0x554u && val == 0x55u && c->nor_unlock == 1) { c->nor_unlock = 2; return; }
	if (c->nor_unlock == 2) {
		if (wo == 0xAAAu) {                     // command cycle at the unlock address
			switch (val) {
				case 0x80: c->nor_erase_armed = 1; c->nor_unlock = 0; return; // erase setup
				case 0xA0: c->nor_prog_armed = 1;  c->nor_unlock = 0; return; // single-word program
				case 0x90: // autoselect: expose device id at win+0x1C
					c->nor_id_mode = 1;
					nor_w16(c, base + 0x1Cu, NOR_DEVICE_ID);
					c->nor_unlock = 0;
					TRACE("NOR autoselect (id=0x%04X)\n", NOR_DEVICE_ID);
					return;
				default: c->nor_unlock = 0; return;
			}
		}
		if (val == 0x25u) {                     // write-to-buffer command at the buffer base
			c->nor_buf_state = NOR_BUF_COUNT;
			c->nor_unlock = 0;
			return;
		}
		c->nor_unlock = 0;
		return;
	}
	if (val == 0xF0) {                          // reset out of id/cmd mode
		if (c->nor_id_mode) { nor_w16(c, base + 0x1Cu, 0xFFFFu); c->nor_id_mode = 0; }
		c->nor_unlock = c->nor_erase_armed = c->nor_prog_armed = 0;
		c->nor_buf_state = NOR_BUF_IDLE;
		return;
	}
	// erase 0xAA/0x55 second-unlock before 0x30 keeps nor_unlock churning; ignore.
	c->nor_unlock = 0;
}

int ez_cart_write(EzCart* c, uint32_t addr, uint32_t val32, int width) {
	uint16_t val = (uint16_t) val32;          // EZ registers are 16-bit
	switch (addr) {
		case R_SD_LBA_LO: c->sd_lba = (c->sd_lba & 0xFFFF0000u) | val; return 1;
		case R_SD_LBA_HI: c->sd_lba = (c->sd_lba & 0x0000FFFFu) | ((uint32_t) val << 16); return 1;
		case R_SD_COUNT:
			c->sd_count = val;
			if (val & 0x8000u) c->sd_pending_write = 1; else c->sd_pending_read = 1;
			return 1;
		case R_SD_CTRL:
			if (val == 3) {                    // status/read-state: stage data now
				if (c->sd_pending_read) { sd_read_into_window(c); c->sd_pending_read = 0; }
				else if (!c->sd_pending_write) cart_w16(c, EZ_DATAWIN_OFF, 0x0000); // ready
			} else if (val == 1) {
				if (c->sd_pending_write) { sd_write_from_window(c); c->sd_pending_write = 0; }
			}
			return 1;
		case R_SPI_CTRL:
			if (val == 1) cart_w16(c, EZ_DATAWIN_OFF, 0xC003); // FPGA ver V03, high nibble 0xC
			return 1;
		case R_SPI_WR: return 1;
		case R_ROM_PG:
			c->mode_os = (val & 1);            // SYSTEM_MODE_OS=1 -> kernel, GAME=0 -> game
			c->rom_page = (uint16_t) (val & ~1u);
			TRACE("ROM page = 0x%04X (mode=%s)\n", val, c->mode_os ? "OS" : "GAME");
			return 1;
		case R_PSRAM_PG: case R_RAM_PG: case R_BUFCTRL: case R_RTC: return 1;
		default: break;
	}
	if (addr >= NORWIN_BASE && addr < NORWIN_END) { nor_write(c, addr, val); return 1; }
	if (addr >= DATAWIN_BASE && addr < DATAWIN_END) {
		// SD write payload DMA'd in by the guest: capture into our window copy.
		uint32_t off = EZ_DATAWIN_OFF + (addr - DATAWIN_BASE);
		if (width == 4) { cart_w16(c, off, (uint16_t) val32); cart_w16(c, off + 2, (uint16_t) (val32 >> 16)); }
		else cart_w16(c, off, val);
		return 1;
	}
	// 0x08xxxxxx knock writes and 0x09FC/0x09FE commit writes: harmless, swallow.
	if ((addr & 0xFF000000u) == 0x08000000u) return 1;
	if (addr == 0x09FC0000u || addr == 0x09FE0000u) return 1;
	return 0;
}

uint32_t ez_cart_read(EzCart* c, uint32_t addr, int width) {
	// Provided for hosts that route data reads through the cart. Honors the same
	// banking as ez_cart_map so a redirected region reads its backing.
	EzCartMap m = ez_cart_map(c, addr);
	uint32_t off = addr & (m.size - 1u);
	const uint8_t* p = m.base;
	if (width == 1) return p[off];
	if (width == 2) { off &= ~1u; return (uint32_t) (p[off] | (p[off + 1] << 8)); }
	off &= ~3u;
	return (uint32_t) (p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) | ((uint32_t) p[off + 3] << 24));
}

EzCartMap ez_cart_map(EzCart* c, uint32_t addr) {
	if (c->mode_os) {
		// OS mode: the 8 MB NOR window reads from the linear backing at the paged
		// base (window-relative offset). Everything else (kernel ROM at 0x08, the
		// data window, register addresses) is identity into the cart buffer.
		if (addr >= NORWIN_BASE && addr < NORWIN_END) {
			EzCartMap m = { c->nor + os_nor_base(c->rom_page), NORWIN_SIZE, 0 };
			return m;
		}
	} else {
		// GAME mode: 0x08000000 maps the NOR game at (rompage << 17). An empty bank
		// (no game burned, e.g. Read_FPGA_ver's bogus page 0x40) banks in the
		// self-branch fill page so the CPU wedges deterministically.
		if (addr >= 0x08000000u && addr <= 0x09FFFFFFu) {
			uint32_t game_off = (uint32_t) c->rom_page << 17;
			int empty = nor_r16(c, game_off) == 0xFFFF && nor_r16(c, game_off + 2u) == 0xFFFF;
			if (!empty && game_off + EZ_CART_SIZE <= c->nor_size) {
				EzCartMap g = { c->nor + game_off, EZ_CART_SIZE, 0 };
				return g;
			}
			EzCartMap f = { c->fill_page, c->fill_size, 0 };
			return f;
		}
	}
	EzCartMap id = { c->cart, EZ_CART_SIZE, 0 };
	return id;
}

uint16_t ez_cart_nor_read16(EzCart* c, uint32_t nor_off) {
	// Settings/game-list live at page-0 (flash0) absolute offsets.
	return nor_r16(c, nor_off);
}

void ez_cart_set_trace(EzCart* c, int on) { c->trace = on; }

EzCart* ez_cart_create(const EzCartConfig* cfg) {
	EzCart* c = (EzCart*) calloc(1, sizeof *c);
	if (!c) return NULL;
	c->cart = cfg->cart;
	c->mode_os = ROM_MODE_OS;
	if (cfg->sd_path) {
		strncpy(c->sd_path, cfg->sd_path, sizeof c->sd_path - 1);
	}

	c->nor_size = EZ_NOR_SIZE;
	c->nor = (uint8_t*) malloc(c->nor_size);
	if (!c->nor) { free(c); return NULL; }
	memset(c->nor, 0xFF, c->nor_size);   // erased flash

	uint16_t* fp = (uint16_t*) malloc(EZ_FILL_SIZE);
	if (!fp) { free(c->nor); free(c); return NULL; }
	for (uint32_t i = 0; i < EZ_FILL_SIZE / 2u; i++) fp[i] = THUMB_SELF_BRANCH;
	c->fill_page = (const uint8_t*) fp;
	c->fill_size = EZ_FILL_SIZE;
	return c;
}

void ez_cart_reset(EzCart* c) {
	// NOR/SD contents persist (caller does not reload them); only the transient
	// register/command state and the page/mode return to power-on.
	c->sd_lba = 0;
	c->sd_count = 0;
	c->sd_pending_read = c->sd_pending_write = 0;
	c->nor_unlock = c->nor_id_mode = c->nor_erase_armed = c->nor_prog_armed = 0;
	c->nor_buf_state = NOR_BUF_IDLE;
	c->nor_buf_remaining = 0;
	c->mode_os = ROM_MODE_OS;
	c->rom_page = 0;
}

void ez_cart_destroy(EzCart* c) {
	if (!c) return;
	free((void*) c->fill_page);
	free(c->nor);
	free(c);
}
