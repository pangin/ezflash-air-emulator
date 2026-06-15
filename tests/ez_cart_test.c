// Unit tests for the EZ-FLASH Air cartridge model (ez_cart.c).
//
// These link ONLY against ez_cart (no mGBA, no GBA core) — the payoff of the
// hexagonal split: the cart hardware model is exercised directly, in seconds,
// without an emulator. A tiny assert harness keeps it dependency-free.

#include "ez_cart.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "  FAIL: %s\n", (msg)); g_fail++; } \
	else { fprintf(stderr, "  ok:   %s\n", (msg)); } \
} while (0)

// GBA cart-space addresses used by the kernel/device.
#define NORWIN       0x09000000u
#define UNLOCK1      (NORWIN + 0xAAAu)   // FlashBase + 0x555*2
#define UNLOCK2      (NORWIN + 0x554u)   // FlashBase + 0x2AA*2
#define R_ROM_PG     0x09880000u
#define R_SPI_CTRL   0x09660000u
#define R_SD_LBA_LO  0x09600000u
#define R_SD_COUNT   0x09640000u
#define R_SD_CTRL    0x09400000u

static EzCart* make_cart(uint8_t* buf, const char* sd) {
	EzCartConfig cfg = { .cart = buf, .sd_path = sd };
	return ez_cart_create(&cfg);
}

// Issue the AMD/Spansion two-cycle unlock used before every command.
static void nor_unlock(EzCart* c) {
	ez_cart_write(c, UNLOCK1, 0xAA, 2);
	ez_cart_write(c, UNLOCK2, 0x55, 2);
}

static void test_nor_autoselect(uint8_t* buf) {
	EzCart* c = make_cart(buf, NULL);
	nor_unlock(c);
	ez_cart_write(c, UNLOCK1, 0x90, 2);                 // autoselect
	CHECK(ez_cart_nor_read16(c, 0x1C) == 0x223D, "NOR autoselect exposes device id 0x223D at win+0x1C");
	ez_cart_write(c, NORWIN, 0xF0, 2);                  // reset out of id mode
	CHECK(ez_cart_nor_read16(c, 0x1C) == 0xFFFF, "NOR reset (0xF0) clears the id back to 0xFFFF");
	ez_cart_destroy(c);
}

static void test_nor_program_and_erase(uint8_t* buf) {
	EzCart* c = make_cart(buf, NULL);
	// program a word at NOR offset 0x40010
	nor_unlock(c);
	ez_cart_write(c, UNLOCK1, 0xA0, 2);                 // program setup
	ez_cart_write(c, NORWIN + 0x40010u, 0x1234, 2);     // data cycle
	CHECK(ez_cart_nor_read16(c, 0x40010) == 0x1234, "NOR word program writes the value");

	// erase the 128 KB sector containing it -> all 0xFF
	nor_unlock(c);
	ez_cart_write(c, UNLOCK1, 0x80, 2);                 // erase setup
	nor_unlock(c);
	ez_cart_write(c, NORWIN + 0x40000u, 0x30, 2);       // sector-erase confirm @ sector base
	CHECK(ez_cart_nor_read16(c, 0x40010) == 0xFFFF, "NOR sector erase clears the programmed word");
	CHECK(ez_cart_nor_read16(c, 0x40000) == 0xFFFF, "NOR sector erase clears the sector base");
	ez_cart_destroy(c);
}

static void test_spi_version(uint8_t* buf) {
	EzCart* c = make_cart(buf, NULL);
	ez_cart_write(c, R_SPI_CTRL, 1, 2);                 // SPI enable -> stage FPGA ver
	CHECK(ez_cart_read(c, 0x09E00000u, 2) == 0xC003, "SPI control=1 stages FPGA version 0xC003");
	ez_cart_destroy(c);
}

static void test_mode_banking(uint8_t* buf) {
	EzCart* c = make_cart(buf, NULL);

	// GAME mode (MODE bit = val & 1 == 0): 0x08 region banks off the cart.
	ez_cart_write(c, R_ROM_PG, 0x0040, 2);
	EzCartMap g = ez_cart_map(c, 0x08000000u);
	CHECK(g.base != buf && !g.is_mmio, "GAME mode: 0x08000000 remapped off the cart buffer");
	CHECK(g.size >= 2 && g.base[0] == 0xFE && g.base[1] == 0xE7,
	      "GAME mode fill page is the Thumb self-branch 0xE7FE (LE)");

	// OS mode (val & 1 == 1): identity into the cart buffer.
	ez_cart_write(c, R_ROM_PG, 0x0001, 2);
	EzCartMap o = ez_cart_map(c, 0x08000000u);
	CHECK(o.base == buf && o.size == EZ_CART_SIZE && !o.is_mmio,
	      "OS mode: 0x08000000 maps identity to the cart buffer");

	// Non-cart addresses are always identity, regardless of mode.
	ez_cart_write(c, R_ROM_PG, 0x0040, 2);              // back to GAME
	EzCartMap w = ez_cart_map(c, 0x02000000u);
	CHECK(w.base == buf, "non-cart address (WRAM) maps identity regardless of mode");

	// reset returns to OS mode.
	ez_cart_reset(c);
	EzCartMap r = ez_cart_map(c, 0x08000000u);
	CHECK(r.base == buf, "ez_cart_reset returns to OS mode");
	ez_cart_destroy(c);
}

static void test_sd_roundtrip(uint8_t* buf) {
	const char* path = "/tmp/ez_cart_test_sd.bin";
	uint8_t pattern[EZ_SD_SECTOR];
	for (int i = 0; i < (int) EZ_SD_SECTOR; i++) pattern[i] = (uint8_t) (i * 7 + 1);
	FILE* f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "  SKIP: cannot create %s\n", path); return; }
	fwrite(pattern, 1, EZ_SD_SECTOR, f);
	fclose(f);

	EzCart* c = make_cart(buf, path);
	// read sector 0 into the data window
	ez_cart_write(c, R_SD_LBA_LO, 0, 2);
	ez_cart_write(c, R_SD_COUNT, 1, 2);                 // 1 block, read direction
	ez_cart_write(c, R_SD_CTRL, 3, 2);                  // stage the read
	CHECK(memcmp(buf + EZ_DATAWIN_OFF, pattern, EZ_SD_SECTOR) == 0,
	      "SD read pulls sector 0 into the data window");

	// modify the window and write it back
	uint8_t pattern2[EZ_SD_SECTOR];
	for (int i = 0; i < (int) EZ_SD_SECTOR; i++) pattern2[i] = (uint8_t) (0xFF - i);
	memcpy(buf + EZ_DATAWIN_OFF, pattern2, EZ_SD_SECTOR);
	ez_cart_write(c, R_SD_LBA_LO, 0, 2);
	ez_cart_write(c, R_SD_COUNT, 0x8001, 2);            // 1 block, write direction
	ez_cart_write(c, R_SD_CTRL, 1, 2);                  // commit the write

	uint8_t check[EZ_SD_SECTOR];
	f = fopen(path, "rb");
	size_t n = fread(check, 1, EZ_SD_SECTOR, f);
	fclose(f);
	CHECK(n == EZ_SD_SECTOR && memcmp(check, pattern2, EZ_SD_SECTOR) == 0,
	      "SD write flushes the data window back to the image");

	ez_cart_destroy(c);
	remove(path);
}

int main(void) {
	uint8_t* buf = (uint8_t*) malloc(EZ_CART_SIZE);
	if (!buf) { fprintf(stderr, "oom\n"); return 2; }

	fprintf(stderr, "[ez_cart_test] NOR autoselect/reset\n");        memset(buf, 0xFF, EZ_CART_SIZE); test_nor_autoselect(buf);
	fprintf(stderr, "[ez_cart_test] NOR program/erase\n");           memset(buf, 0xFF, EZ_CART_SIZE); test_nor_program_and_erase(buf);
	fprintf(stderr, "[ez_cart_test] SPI version\n");                 memset(buf, 0xFF, EZ_CART_SIZE); test_spi_version(buf);
	fprintf(stderr, "[ez_cart_test] mode/banking map\n");            memset(buf, 0xFF, EZ_CART_SIZE); test_mode_banking(buf);
	fprintf(stderr, "[ez_cart_test] SD read/write roundtrip\n");     memset(buf, 0xFF, EZ_CART_SIZE); test_sd_roundtrip(buf);

	free(buf);
	if (g_fail) { fprintf(stderr, "\n%d check(s) FAILED\n", g_fail); return 1; }
	fprintf(stderr, "\nall checks passed\n");
	return 0;
}
