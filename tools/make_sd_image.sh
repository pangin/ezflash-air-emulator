#!/usr/bin/env bash
# Build a raw FAT32 SD image with Korean/ASCII-named dummy ROMs for the harness.
# Usage: tools/make_sd_image.sh [out.img]
set -euo pipefail

OUT="${1:-work/sd.img}"
SIZE_MB=64
VOL=EZTEST
mkdir -p "$(dirname "$OUT")"

# Dummy ROM files to place at the SD root. The kernel lists every file; the
# extension only matters at launch. Korean LFNs are stored as UTF-16 on disk
# regardless of host OS, so FatFs (FF_LFN_UNICODE=2) returns them as UTF-8.
make_dummies() {
	local dir="$1"
	# minimal valid-ish GBA header (branch + zeroed logo area is enough to list)
	printf '\x2e\x00\x00\xea' > "$dir/한글테스트게임.gba"
	head -c 4096 /dev/zero >> "$dir/한글테스트게임.gba"
	printf 'NES\x1a' > "$dir/테스트.nes"; head -c 2048 /dev/zero >> "$dir/테스트.nes"
	printf '\x2e\x00\x00\xea' > "$dir/Sample Game.gba"; head -c 4096 /dev/zero >> "$dir/Sample Game.gba"
	printf '\x00' > "$dir/포켓몬 루비.gba"; head -c 8192 /dev/zero >> "$dir/포켓몬 루비.gba"
	mkdir -p "$dir/SAVER"
}

if [[ "$(uname)" == "Darwin" ]]; then
	rm -f "$OUT"
	# Raw image, attach without mounting, format FAT32, mount, copy, detach.
	dd if=/dev/zero of="$OUT" bs=1m count="$SIZE_MB" status=none
	DEV=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$OUT" | head -1 | awk '{print $1}')
	echo "attached $DEV"
	newfs_msdos -F 32 -v "$VOL" "$DEV" >/dev/null
	diskutil mount "$DEV" >/dev/null
	MP="/Volumes/$VOL"
	make_dummies "$MP"
	ls -la "$MP"
	diskutil unmount "$DEV" >/dev/null
	hdiutil detach "$DEV" >/dev/null
	echo "made $OUT ($(du -h "$OUT" | cut -f1)) FAT32"
else
	# Linux/CI: mtools, no root needed.
	command -v mkfs.vfat >/dev/null || { echo "need dosfstools"; exit 1; }
	command -v mcopy >/dev/null || { echo "need mtools"; exit 1; }
	rm -f "$OUT"
	dd if=/dev/zero of="$OUT" bs=1M count="$SIZE_MB" status=none
	mkfs.vfat -F 32 -n "$VOL" "$OUT" >/dev/null
	TMP=$(mktemp -d); make_dummies "$TMP"
	export MTOOLS_SKIP_CHECK=1
	for f in "$TMP"/*; do
		[[ -d "$f" ]] && mmd -i "$OUT" "::$(basename "$f")" || mcopy -i "$OUT" "$f" "::$(basename "$f")"
	done
	rm -rf "$TMP"
	echo "made $OUT FAT32"
fi
