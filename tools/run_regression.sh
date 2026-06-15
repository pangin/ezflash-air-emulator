#!/usr/bin/env bash
# Advanced regression suite (beyond the scenario sweep in tools/run_tests.sh):
#   1. help-freeze paired regression — buggy kernel freezes, fixed kernel renders
#   2. game-boot end-to-end       — load a real ROM from SD into NOR and boot it
#
# The fixed-kernel halves are CI-runnable (CI builds the fixed kernel); the buggy
# half needs the pre-fix kernel fixture (work/ezairkernel_buggy.bin) and is
# skipped if absent.  Usage: tools/run_regression.sh [fixed.bin] [buggy.bin]
set -euo pipefail
cd "$(dirname "$0")/.."

RUNNER=build/super/ezair-runner
FIXED="${1:-work/ezairkernel.bin}"
BUGGY="${2:-work/ezairkernel_buggy.bin}"
[[ -x "$RUNNER" ]] || { echo "runner not built; run: ninja -C build/super ezair-runner"; exit 2; }
[[ -f "$FIXED" ]] || { echo "fixed kernel not found: $FIXED"; exit 2; }
[[ -f work/sd.img ]]      || bash tools/make_sd_image.sh work/sd.img
[[ -f work/sd_game.img ]] || bash tools/make_game_sd.sh work/sd_game.img
mkdir -p out
fail=0

echo "──────── help freeze: buggy kernel (expect frozen) ────────"
if [[ -f "$BUGGY" ]]; then
	"$RUNNER" "$BUGGY" --sd work/sd.img --scenario tests/regression/help_freeze_buggy.txt --out out || { echo "FAIL: buggy did not freeze"; fail=1; }
else
	echo "  (skip: no buggy fixture $BUGGY)"
fi
echo "──────── help freeze: fixed kernel (expect rendered) ────────"
"$RUNNER" "$FIXED" --sd work/sd.img --scenario scenarios/help_fixed.txt --out out || { echo "FAIL: fixed did not render help"; fail=1; }
echo "──────── game boot e2e: load + boot a NOR game (expect green) ────────"
"$RUNNER" "$FIXED" --sd work/sd_game.img --scenario tests/regression/game_boot.txt --out out || { echo "FAIL: game did not boot"; fail=1; }

echo "════════════════════════════════"
[[ $fail -eq 0 ]] && echo "✅ ALL ADVANCED REGRESSIONS PASSED" || echo "❌ ADVANCED REGRESSIONS FAILED"
exit $fail
