// Headless EZ-FLASH Air test runner.
//
//   ezair-runner <kernel.bin> [--sd <fat.img>] [--scenario <file>] [--out <dir>] [--trace]
//
// Loads the kernel into a 32 MB cart buffer (so all cart-space reads land in a
// buffer we own), attaches the EZ-FLASH Air device model, then executes a
// scenario script driving keys and dumping framebuffers as PPM.
//
// Scenario commands (one per line; '#' comments):
//   keys <mask>            set held keys (GBA bits: A1 B2 SEL4 START8 R16 L32 U64 D128 R256 L512)
//   frames <n>             run n frames with current keys
//   tap <mask> [hold] [gap]  press mask for `hold` frames, release for `gap` (default 6/6)
//   reset                  core reset (cart NOR/SD state persists)
//   shot <name>            write <out>/<name>.ppm
//   assert_nor16 <off> <v> assert persisted NOR window u16 at hex offset == hex value
//   assert_nonblank <name> <x> <y> <w> <h>  fail if region is uniform (un-rendered)
//   log <text>             print a marker
//
// Exit code: 0 all asserts passed, nonzero otherwise.

#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/core/config.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>
#include <mgba/internal/gba/gba.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ezair_device.h"

#define GBA_W 240
#define GBA_H 160
#define FB_STRIDE 256

// Silence mGBA's default logger (DMA/SWI/STUB chatter floods stderr).
static void noop_log(struct mLogger* l, int cat, enum mLogLevel lvl, const char* fmt, va_list a) {
	(void) l; (void) cat; (void) lvl; (void) fmt; (void) a;
}
static struct mLogger s_logger = { .log = noop_log };

static struct mCore* s_core;
static color_t s_fb[FB_STRIDE * GBA_H];
static uint32_t s_keys;
static char s_out[1024] = "out";
static int s_fails;

static void run_frames(int n) {
	for (int i = 0; i < n; ++i) {
		s_core->setKeys(s_core, s_keys);
		s_core->runFrame(s_core);
	}
}

static const color_t* frame_pixels(size_t* stride) {
	const void* px;
	s_core->getPixels(s_core, &px, stride);
	return (const color_t*) px;
}

// color_t here is uint32_t (this build defines no COLOR_16_BIT): byte0=R,1=G,2=B.
static void write_ppm(const char* name) {
	size_t stride;
	const color_t* px = frame_pixels(&stride);
	char path[1200];
	snprintf(path, sizeof path, "%s/%s.ppm", s_out, name);
	FILE* f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "cannot write %s\n", path); s_fails++; return; }
	fprintf(f, "P6\n%d %d\n255\n", GBA_W, GBA_H);
	for (int y = 0; y < GBA_H; ++y) {
		for (int x = 0; x < GBA_W; ++x) {
			uint32_t p = px[y * stride + x];
			unsigned char rgb[3] = { (unsigned char) p, (unsigned char)(p >> 8), (unsigned char)(p >> 16) };
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	fprintf(stderr, "[shot] %s\n", path);
}

static int region_uniform(int x, int y, int w, int h) {
	size_t stride;
	const color_t* px = frame_pixels(&stride);
	color_t first = px[y * stride + x];
	for (int j = y; j < y + h; ++j)
		for (int i = x; i < x + w; ++i)
			if (px[j * stride + i] != first) return 0;
	return 1;
}

static uint32_t hexnum(const char* s) { return (uint32_t) strtoul(s, NULL, 0); }

static void exec_line(char* line) {
	char* tok = strtok(line, " \t\r\n");
	if (!tok || tok[0] == '#') return;

	if (!strcmp(tok, "keys")) {
		s_keys = hexnum(strtok(NULL, " \t\r\n"));
	} else if (!strcmp(tok, "frames")) {
		run_frames((int) hexnum(strtok(NULL, " \t\r\n")));
	} else if (!strcmp(tok, "tap")) {
		uint32_t mask = hexnum(strtok(NULL, " \t\r\n"));
		char* a = strtok(NULL, " \t\r\n"); char* b = strtok(NULL, " \t\r\n");
		int hold = a ? (int) hexnum(a) : 6, gap = b ? (int) hexnum(b) : 6;
		uint32_t saved = s_keys;
		s_keys = saved | mask; run_frames(hold);
		s_keys = saved;        run_frames(gap);
	} else if (!strcmp(tok, "reset")) {
		s_core->reset(s_core);
	} else if (!strcmp(tok, "shot")) {
		write_ppm(strtok(NULL, " \t\r\n"));
	} else if (!strcmp(tok, "assert_nor16")) {
		uint32_t off = hexnum(strtok(NULL, " \t\r\n"));
		uint16_t want = (uint16_t) hexnum(strtok(NULL, " \t\r\n"));
		uint16_t got = ezair_nor_read16(off);
		if (got != want) { fprintf(stderr, "[FAIL] NOR[0x%X]=0x%04X want 0x%04X\n", off, got, want); s_fails++; }
		else fprintf(stderr, "[ok] NOR[0x%X]=0x%04X\n", off, got);
	} else if (!strcmp(tok, "assert_nonblank")) {
		char* name = strtok(NULL, " \t\r\n");
		int x = (int) hexnum(strtok(NULL, " \t\r\n")), y = (int) hexnum(strtok(NULL, " \t\r\n"));
		int w = (int) hexnum(strtok(NULL, " \t\r\n")), h = (int) hexnum(strtok(NULL, " \t\r\n"));
		if (region_uniform(x, y, w, h)) { fprintf(stderr, "[FAIL] %s: region uniform (blank)\n", name); s_fails++; }
		else fprintf(stderr, "[ok] %s: rendered\n", name);
	} else if (!strcmp(tok, "log")) {
		fprintf(stderr, "[log] %s\n", strtok(NULL, "\r\n") ? tok + 4 : "");
	} else {
		fprintf(stderr, "unknown scenario command: %s\n", tok);
	}
}

int main(int argc, char** argv) {
	const char *kernel = NULL, *sd = NULL, *scenario = NULL;
	int trace = 0;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--sd") && i + 1 < argc) sd = argv[++i];
		else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) strncpy(s_out, argv[++i], sizeof(s_out) - 1);
		else if (!strcmp(argv[i], "--trace")) trace = 1;
		else if (argv[i][0] != '-') kernel = argv[i];
	}
	if (!kernel) { fprintf(stderr, "usage: ezair-runner <kernel.bin> [--sd img] [--scenario f] [--out dir] [--trace]\n"); return 2; }

	// Load kernel into a 32 MB cart buffer, 0xFF-padded (erased-NOR default).
	uint8_t* cart = malloc(EZ_CART_SIZE);
	if (!cart) { fprintf(stderr, "oom\n"); return 2; }
	memset(cart, 0xFF, EZ_CART_SIZE);
	FILE* kf = fopen(kernel, "rb");
	if (!kf) { fprintf(stderr, "cannot open kernel %s\n", kernel); return 2; }
	size_t kn = fread(cart, 1, EZ_CART_SIZE, kf);
	fclose(kf);
	fprintf(stderr, "[runner] kernel %zu bytes into 32MB cart\n", kn);

	mLogSetDefaultLogger(&s_logger);
	s_core = GBACoreCreate();
	s_core->init(s_core);
	s_core->setVideoBuffer(s_core, s_fb, FB_STRIDE);

	struct VFile* vf = VFileFromMemory(cart, EZ_CART_SIZE);
	if (!s_core->loadROM(s_core, vf)) { fprintf(stderr, "loadROM failed\n"); return 2; }

	struct GBA* gba = (struct GBA*) s_core->board;
	fprintf(stderr, "[runner] rom=%p cart=%p romSize=0x%X (alias=%d)\n",
		(void*) gba->memory.rom, (void*) cart, gba->memory.romSize,
		gba->memory.rom == cart);
	// Ensure full 32MB coverage so 0x09xxxxxx reads hit our buffer.
	if (gba->memory.rom != cart) cart = gba->memory.rom; // attach to the live buffer
	gba->memory.romSize = EZ_CART_SIZE;
	gba->memory.romMask = EZ_CART_SIZE - 1;

	mCoreConfigInit(&s_core->config, "ezair");
	ezair_set_trace(trace);
	ezair_attach(s_core, cart, sd);
	s_core->reset(s_core);

	if (scenario) {
		FILE* sf = fopen(scenario, "r");
		if (!sf) { fprintf(stderr, "cannot open scenario %s\n", scenario); return 2; }
		char line[2048];
		while (fgets(line, sizeof line, sf)) exec_line(line);
		fclose(sf);
	} else {
		run_frames(600);
		write_ppm("boot");
	}

	fprintf(stderr, s_fails ? "[runner] %d assertion(s) FAILED\n" : "[runner] all assertions passed\n", s_fails);
	return s_fails ? 1 : 0;
}
