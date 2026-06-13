// EZ-FLASH Air cartridge device model for mGBA.
//
// The real cart routes GBA cartridge-bus accesses through an FPGA. The kernel
// talks to it with fixed-address 16-bit register writes (wrapped in a "magic
// knock") and reads results back through a shared data window at 0x09E00000 and
// the NOR window at 0x09000000. We model this by:
//   * forcing the cart ROM buffer to a full 32 MB so every cart-space read
//     lands in a buffer we own (gba->memory.rom alias), then PRE-POKING the
//     right bytes before the guest reads them (no read hooks needed);
//   * hooking ARM store8/16/32 to intercept register writes, NOR JEDEC
//     commands, and SD write data.
//
// What is modeled: SD block read/write (against a FAT image file), NOR JEDEC
// erase/word-program/autoselect-ID over the 0x09000000 window (so settings and
// the NOR game list persist across guest resets, since the cart buffer is not
// reloaded on reset), and the SPI FPGA-version read.
// What is NOT modeled: booting a game into GAME mode, FPGA SPI re-flashing,
// PSRAM/save bank switching (accepted and logged, no behavior).

#ifndef EZAIR_DEVICE_H
#define EZAIR_DEVICE_H

#include <stdint.h>
#include <stddef.h>

struct mCore;

#define EZ_CART_SIZE   0x02000000u      // 32 MB cart address space (SIZE_CART0)
#define EZ_NORWIN_OFF  0x01000000u      // cart offset of the 0x09000000 NOR window
#define EZ_DATAWIN_OFF 0x01E00000u      // cart offset of the 0x09E00000 data window
#define EZ_SD_SECTOR   512u

// Attach the device model to an initialized core whose ROM is the 32 MB buffer
// `cart`. `sd_path` is a raw FAT image file (read/write); may be NULL.
// Returns 0 on success.
int ezair_attach(struct mCore* core, uint8_t* cart, const char* sd_path);

// Verbose register/command tracing to stderr.
void ezair_set_trace(int on);

// Read a 16-bit little-endian word from the persisted NOR window (cart buffer),
// e.g. to assert the saved language at NOR offset 0x7C0000. `nor_off` is the
// offset within the NOR window (0x09000000-relative).
uint16_t ezair_nor_read16(uint32_t nor_off);

#endif
