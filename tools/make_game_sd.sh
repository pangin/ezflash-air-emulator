#!/usr/bin/env bash
# Build a raw FAT32 SD image holding one real, bootable test ROM for the
# game-boot end-to-end test. The kernel loads it from SD into NOR and boots it.
# Usage: tools/make_game_sd.sh [out.img]
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="${1:-work/sd_game.img}"
ROM="tests/romsrc/testrom.gba"
SIZE_MB=64
VOL=EZGAME
mkdir -p "$(dirname "$OUT")"
[[ -f "$ROM" ]] || { echo "test ROM not found: $ROM (build it in tests/romsrc/)"; exit 2; }

populate() {
	local dir="$1"
	cp "$ROM" "$dir/testrom.gba"   # the only file -> SD list: SAVER folder, then testrom.gba
	mkdir -p "$dir/SAVER"
}

if [[ "$(uname)" == "Darwin" ]]; then
	rm -f "$OUT"
	dd if=/dev/zero of="$OUT" bs=1m count="$SIZE_MB" status=none
	DEV=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$OUT" | head -1 | awk '{print $1}')
	newfs_msdos -F 32 -v "$VOL" "$DEV" >/dev/null
	diskutil mount "$DEV" >/dev/null
	MP="/Volumes/$VOL"
	populate "$MP"
	ls -la "$MP"
	diskutil unmount "$DEV" >/dev/null
	hdiutil detach "$DEV" >/dev/null
	echo "made $OUT FAT32"
else
	command -v mkfs.vfat >/dev/null || { echo "need dosfstools"; exit 1; }
	command -v mcopy >/dev/null || { echo "need mtools"; exit 1; }
	rm -f "$OUT"
	dd if=/dev/zero of="$OUT" bs=1M count="$SIZE_MB" status=none
	mkfs.vfat -F 32 -n "$VOL" "$OUT" >/dev/null
	export MTOOLS_SKIP_CHECK=1
	mmd -i "$OUT" "::SAVER"
	mcopy -i "$OUT" "$ROM" "::testrom.gba"
	echo "made $OUT FAT32"
fi
