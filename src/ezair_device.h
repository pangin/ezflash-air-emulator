// mGBA-facing facade for the EZ-FLASH Air device model.
//
// The cartridge hardware itself lives in the core-agnostic ez_cart.* module; the
// mGBA glue lives in ez_mgba_adapter.c. This header is the small surface the
// runner uses to attach the device to a core and to inspect persisted NOR — it
// is kept stable so the runner does not need to know about the cart/adapter
// split. The EZ_* layout constants come from ez_cart.h (single source of truth).

#ifndef EZAIR_DEVICE_H
#define EZAIR_DEVICE_H

#include <stdint.h>
#include "ez_cart.h"

struct mCore;

// Attach the device model to an initialized core whose ROM is the 32 MB buffer
// `cart`. `sd_path` is a raw FAT image file (read/write); may be NULL.
// Returns 0 on success.
int ezair_attach(struct mCore* core, uint8_t* cart, const char* sd_path);

// Verbose register/command tracing to stderr. May be called before ezair_attach.
void ezair_set_trace(int on);

// Read a 16-bit little-endian word from the persisted NOR window (cart buffer),
// e.g. to assert the saved language at NOR offset 0x7C0000. `nor_off` is the
// offset within the NOR window (0x09000000-relative).
uint16_t ezair_nor_read16(uint32_t nor_off);

#endif
