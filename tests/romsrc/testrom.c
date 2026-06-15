// Minimal bootable GBA ROM for the EZ-FLASH Air game-boot test.
//
// It fills the mode-3 framebuffer with a solid distinctive green and loops, so a
// scenario can assert the game actually booted from NOR (the screen is the game's
// green, not the kernel's blue menu). Deliberately tiny: no save, no libgba calls,
// just register/VRAM writes, so booting it exercises only the GAME-mode bank
// switch — not the kernel's save/patch machinery.

#define REG_DISPCNT (*(volatile unsigned int*)  0x04000000)
#define VRAM        ( (volatile unsigned short*) 0x06000000)
#define MODE3_BG2   0x0403u            // MODE_3 | BG2_ON
#define GREEN       0x03E0u            // BGR555 green

int main(void) {
	REG_DISPCNT = MODE3_BG2;
	for (int i = 0; i < 240 * 160; i++) VRAM[i] = GREEN;
	for (;;) { }
	return 0;
}
