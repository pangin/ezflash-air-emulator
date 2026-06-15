// mGBA adapter for the EZ-FLASH Air cartridge model.
//
// This is the ONLY translation unit that includes mGBA headers. It owns the
// glue between mGBA's function-pointer memory interface and the core-agnostic
// cartridge model in ez_cart.c: it installs runtime hooks on cpu->memory.* and
// forwards each access to ez_cart_*. Swapping the GBA core means rewriting only
// this file; the cartridge model is untouched. mGBA itself stays unmodified.
//
//   * store8/16/32  -> ez_cart_write (register/NOR/SD writes)
//   * setActiveRegion / load8/16/32 -> routed through ez_cart_map so OS/GAME bank
//     switching redirects instruction prefetch (and data reads) without touching
//     mGBA. ez_cart_map is the single authority; this file is generic glue.
//
// Banking policy lives entirely in the cart model: when ez_cart_map returns the
// identity mapping (base == the core's cart buffer) we delegate to mGBA's native
// path (preserving its waitstate/idle-loop behavior); only when the cart remaps a
// region to a different buffer do we override.

#include "ez_cart.h"
#include "ezair_device.h"

#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/arm/arm.h>

// ---- glue state (mGBA-specific; not part of the cart model) ----
static struct ARMCore* s_cpu;
static struct GBA*     s_gba;
static EzCart*         s_dev;
static const uint8_t*  s_cart_base;   // identity-map base (== core's ROM buffer)
static int             s_pending_trace;

static void (*orig_store8)(struct ARMCore*, uint32_t, int8_t, int*);
static void (*orig_store16)(struct ARMCore*, uint32_t, int16_t, int*);
static void (*orig_store32)(struct ARMCore*, uint32_t, int32_t, int*);
static uint32_t (*orig_load8)(struct ARMCore*, uint32_t, int*);
static uint32_t (*orig_load16)(struct ARMCore*, uint32_t, int*);
static uint32_t (*orig_load32)(struct ARMCore*, uint32_t, int*);
static uint32_t (*orig_loadMultiple)(struct ARMCore*, uint32_t, int, enum LSMDirection, int*);
static void (*orig_setActiveRegion)(struct ARMCore*, uint32_t);

static inline int is_cart_space(uint32_t addr) {
	return addr >= 0x08000000u && addr <= 0x09FFFFFFu;
}

// ---- write hooks ----
static void hook_store8(struct ARMCore* cpu, uint32_t addr, int8_t value, int* cyc) {
	if (is_cart_space(addr) && ez_cart_write(s_dev, addr, (uint8_t) value, 1)) return;
	orig_store8(cpu, addr, value, cyc);
}
static void hook_store16(struct ARMCore* cpu, uint32_t addr, int16_t value, int* cyc) {
	if (is_cart_space(addr) && ez_cart_write(s_dev, addr, (uint16_t) value, 2)) return;
	orig_store16(cpu, addr, value, cyc);
}
static void hook_store32(struct ARMCore* cpu, uint32_t addr, int32_t value, int* cyc) {
	if (is_cart_space(addr) && ez_cart_write(s_dev, addr, (uint32_t) value, 4)) return;
	orig_store32(cpu, addr, value, cyc);
}

// ---- bank-aware read/fetch routing ----
// True when the cart remaps `addr` to a buffer other than the core's own ROM.
// In that case we serve the access ourselves; otherwise mGBA's native path is
// correct (and faster), so we delegate.
static inline int remapped(const EzCartMap* m) {
	return m->base != s_cart_base;
}

static uint32_t redirect_read(const EzCartMap* m, uint32_t addr, int width) {
	uint32_t off = addr & (m->size - 1u);
	const uint8_t* p = m->base;
	if (width == 1) return p[off];
	if (width == 2) { off &= ~1u; return (uint32_t) (p[off] | (p[off + 1] << 8)); }
	off &= ~3u;
	return (uint32_t) (p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) | ((uint32_t) p[off + 3] << 24));
}

static uint32_t hook_load8(struct ARMCore* cpu, uint32_t addr, int* cyc) {
	if (is_cart_space(addr)) { EzCartMap m = ez_cart_map(s_dev, addr); if (remapped(&m)) return redirect_read(&m, addr, 1); }
	return orig_load8(cpu, addr, cyc);
}
static uint32_t hook_load16(struct ARMCore* cpu, uint32_t addr, int* cyc) {
	if (is_cart_space(addr)) { EzCartMap m = ez_cart_map(s_dev, addr); if (remapped(&m)) return redirect_read(&m, addr, 2); }
	return orig_load16(cpu, addr, cyc);
}
static uint32_t hook_load32(struct ARMCore* cpu, uint32_t addr, int* cyc) {
	if (is_cart_space(addr)) { EzCartMap m = ez_cart_map(s_dev, addr); if (remapped(&m)) return redirect_read(&m, addr, 4); }
	return orig_load32(cpu, addr, cyc);
}

// LDM/LDMIA from a remapped region (e.g. a game's crt0 copying its ROM sections
// in GAME mode). GBALoadMultiple reads memory->rom directly, so we briefly point
// mGBA's ROM view at the mapped buffer for the duration of the multi-load. Note
// memory->rom is indexed by (addr & (SIZE_CART0-1)), and the mapped base already
// accounts for the bank offset, so the indexing lines up.
static uint32_t hook_loadMultiple(struct ARMCore* cpu, uint32_t base, int mask, enum LSMDirection dir, int* cyc) {
	if (is_cart_space(base)) {
		EzCartMap m = ez_cart_map(s_dev, base);
		if (remapped(&m)) {
			uint32_t* save_rom = s_gba->memory.rom;
			uint32_t  save_mask = s_gba->memory.romMask;
			size_t    save_size = s_gba->memory.romSize;
			s_gba->memory.rom = (uint32_t*) m.base;
			s_gba->memory.romMask = m.size - 1u;
			s_gba->memory.romSize = m.size;
			uint32_t r = orig_loadMultiple(cpu, base, mask, dir, cyc);
			s_gba->memory.rom = save_rom;
			s_gba->memory.romMask = save_mask;
			s_gba->memory.romSize = save_size;
			return r;
		}
	}
	return orig_loadMultiple(cpu, base, mask, dir, cyc);
}

// Instruction prefetch: mGBA fetches directly from cpu->memory.activeRegion using
// (PC & activeMask) as a byte offset. To bank the fetch we point activeRegion at
// the cart's mapped buffer; in identity (OS) mode we delegate so mGBA keeps its
// waitstate/idle-loop bookkeeping.
static void hook_setActiveRegion(struct ARMCore* cpu, uint32_t address) {
	if (is_cart_space(address)) {
		EzCartMap m = ez_cart_map(s_dev, address);
		if (remapped(&m)) {
			uint32_t mask = (m.size - 1u) & ((cpu->executionMode == MODE_THUMB) ? ~1u : ~3u);
			cpu->memory.activeRegion = (const uint32_t*) m.base;
			cpu->memory.activeMask = mask;
			s_gba->memory.activeRegion = (int) (address >> 24); // keep mGBA's region int in sync
			return;
		}
	}
	orig_setActiveRegion(cpu, address);
}

int ezair_attach(struct mCore* core, uint8_t* cart, const char* sd_path) {
	struct GBA* gba = (struct GBA*) core->board;
	s_cpu = gba->cpu;
	s_gba = gba;
	s_cart_base = cart;

	EzCartConfig cfg = { .cart = cart, .sd_path = sd_path };
	s_dev = ez_cart_create(&cfg);
	if (!s_dev) return -1;
	ez_cart_set_trace(s_dev, s_pending_trace);

	orig_store8  = s_cpu->memory.store8;
	orig_store16 = s_cpu->memory.store16;
	orig_store32 = s_cpu->memory.store32;
	orig_load8   = s_cpu->memory.load8;
	orig_load16  = s_cpu->memory.load16;
	orig_load32  = s_cpu->memory.load32;
	orig_loadMultiple = s_cpu->memory.loadMultiple;
	orig_setActiveRegion = s_cpu->memory.setActiveRegion;

	s_cpu->memory.store8  = hook_store8;
	s_cpu->memory.store16 = hook_store16;
	s_cpu->memory.store32 = hook_store32;
	s_cpu->memory.load8   = hook_load8;
	s_cpu->memory.load16  = hook_load16;
	s_cpu->memory.load32  = hook_load32;
	s_cpu->memory.loadMultiple = hook_loadMultiple;
	s_cpu->memory.setActiveRegion = hook_setActiveRegion;
	return 0;
}

void ezair_set_trace(int on) {
	s_pending_trace = on;            // remembered if called before attach
	if (s_dev) ez_cart_set_trace(s_dev, on);
}

uint16_t ezair_nor_read16(uint32_t nor_off) {
	return s_dev ? ez_cart_nor_read16(s_dev, nor_off) : 0;
}
