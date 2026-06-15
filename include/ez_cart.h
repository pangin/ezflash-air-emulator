// EZ-FLASH Air cartridge hardware model — host-agnostic port.
//
// This header describes the EZ-FLASH Air cartridge as a memory-mapped device on
// the GBA cartridge bus (0x08000000-0x09FFFFFF). It deliberately knows NOTHING
// about mGBA or any GBA core: a core-specific adapter (see ez_mgba_adapter.c)
// drives the cart through this small interface. That one-way dependency
// (adapter -> cart core) lets the cart model be reused if the GBA core is
// swapped, and unit-tested on its own without an emulator.
//
// The cart reacts to CPU bus writes (ez_cart_write) — register knocks, NOR JEDEC
// commands, SD transfers, SPI, and ROM page/mode switching — and tells the host
// what each cart-space address currently maps to (ez_cart_map), which is the
// authority the adapter uses to route instruction fetches and reads under
// OS/GAME bank switching.

#ifndef EZ_CART_H
#define EZ_CART_H

#include <stdint.h>
#include <stddef.h>

#define EZ_CART_SIZE   0x02000000u      // 32 MB cart address space (SIZE_CART0)
#define EZ_NORWIN_OFF  0x01000000u      // cart offset of the 0x09000000 NOR window
#define EZ_DATAWIN_OFF 0x01E00000u      // cart offset of the 0x09E00000 data window
#define EZ_SD_SECTOR   512u

typedef struct EzCart EzCart;           // opaque cartridge instance

typedef struct {
	uint8_t*    cart;       // 32 MB cart buffer; owned by the caller, not the cart
	const char* sd_path;    // raw FAT image file (read/write); may be NULL
} EzCartConfig;

// What a cart-space address currently maps to, given the active page/mode.
// `base` is a byte pointer the host may read/fetch directly (NULL if MMIO-only);
// the masked address indexes into it (`size` must be a power of two so the host
// can use `size - 1` as a fetch mask). `is_mmio` marks register windows that
// must be serviced through ez_cart_read rather than fetched as memory.
typedef struct {
	const uint8_t* base;
	uint32_t       size;
	int            is_mmio;
} EzCartMap;

// Lifecycle. The cart does NOT take ownership of cfg->cart (the host frees it);
// ez_cart_destroy frees only the cart's own allocations. ez_cart_reset models a
// power-on/soft-reset: NOR and SD contents persist (the buffer is not reloaded),
// but the page/mode returns to OS mode.
EzCart*  ez_cart_create(const EzCartConfig* cfg);
void     ez_cart_reset(EzCart* c);
void     ez_cart_destroy(EzCart* c);

// CPU bus write at a cart-space address. Returns 1 if the device consumed the
// write (the host must NOT also write it through to memory), 0 to pass through.
int      ez_cart_write(EzCart* c, uint32_t addr, uint32_t val, int width);

// CPU bus read of a device MMIO window (for hosts whose data reads do not go
// through ez_cart_map). Returns the value the device presents at `addr`.
uint32_t ez_cart_read(EzCart* c, uint32_t addr, int width);

// Banking authority: what `addr` maps to right now. The host uses this to point
// its instruction-prefetch region (and, for full banking, its data reads).
EzCartMap ez_cart_map(EzCart* c, uint32_t addr);

// Read a 16-bit little-endian word from the persisted NOR window (cart buffer),
// e.g. to assert the saved language at NOR offset 0x7C0000. `nor_off` is the
// offset within the NOR window (0x09000000-relative).
uint16_t ez_cart_nor_read16(EzCart* c, uint32_t nor_off);

// Verbose register/command tracing to stderr.
void     ez_cart_set_trace(EzCart* c, int on);

#endif
