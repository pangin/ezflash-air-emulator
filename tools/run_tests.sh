#!/usr/bin/env bash
# Build (if needed), then run every scenario against the kernel image and
# convert screenshots to PNG. Fails if any scenario's assertions fail.
#
#   tools/run_tests.sh [kernel.bin]
set -euo pipefail
cd "$(dirname "$0")/.."

KERNEL="${1:-work/ezairkernel.bin}"
RUNNER=build/super/ezair-runner
SD=work/sd.img
OUT=out

[[ -f "$KERNEL" ]] || { echo "kernel image not found: $KERNEL"; exit 2; }
[[ -x "$RUNNER" ]] || { echo "runner not built; run: cmake -S . -B build/super -G Ninja && ninja -C build/super ezair-runner"; exit 2; }
[[ -f "$SD" ]] || bash tools/make_sd_image.sh "$SD"
mkdir -p "$OUT"

fail=0
for sc in scenarios/*.txt; do
	name=$(basename "$sc" .txt)
	echo "──────── scenario: $name ────────"
	if "$RUNNER" "$KERNEL" --sd "$SD" --scenario "$sc" --out "$OUT" 2>/dev/null; then
		echo "PASS $name"
	else
		echo "FAIL $name"; fail=1
	fi
done

# PPM -> PNG for human-viewable artifacts (Pillow).
if command -v python3 >/dev/null && python3 -c "import PIL" 2>/dev/null; then
	python3 - <<'PY'
import glob, os
from PIL import Image
for p in glob.glob("out/*.ppm"):
    Image.open(p).save(p[:-4] + ".png")
print("converted", len(glob.glob("out/*.png")), "screenshots to PNG")
PY
fi

echo "════════════════════════════════"
[[ $fail -eq 0 ]] && echo "ALL SCENARIOS PASSED" || echo "SOME SCENARIOS FAILED"
exit $fail
