#!/usr/bin/env bash
# time boot to the title arena under different settings, one screenshot a second
#   tools/boot_timing.sh name "--extra --args" [name "--args" ...]
# runs on the test profile; taps A through language select only
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
taps=$(seq 3 0.5 40 | sed 's/$/:a/' | paste -sd';')
while (($# >= 2)); do
  name=$1 args=$2; shift 2
  out=logs/boot-$name
  rm -rf "$out"
  tools/shots.sh "$out" 1 70 &
  shooter=$!
  # shellcheck disable=SC2086
  BARDOWN_PROFILE=test BARDOWN_INPUT="$taps" timeout 72 scripts/run.sh run --fullscreen=false \
    --mnk_mode=false $args > /dev/null 2>&1
  wait "$shooter" 2>/dev/null
  python3 - "$out" "$name" <<'EOF'
import glob, sys
from PIL import Image, ImageStat
out, name = sys.argv[1:]
fs = sorted(glob.glob(f"{out}/*.png"))
def st(f):
    return [round(x) for x in ImageStat.Stat(Image.open(f).convert("RGB").resize((64, 36))).mean]
# title arena / main menu: dark and blue-leaning. ea logo video: red-dominant frames
def dark_blue(c): return sum(c) / 3 < 80 and c[2] > c[0] + 4
def red(c): return c[0] > c[1] + 25 and c[0] > c[2] + 25
stats = [(f, st(f)) for f in fs]
title = next((f for f, c in stats if dark_blue(c)), None)
reds = [f for f, c in stats if red(c)]
print(f"{name}: {len(fs)} shots, title/menu at {title[-7:-4] + 's' if title else 'not reached'}, "
      f"red logo frames {len(reds)} ({reds[0][-7:-4] if reds else '-'}..{reds[-1][-7:-4] if reds else '-'})")
EOF
  sleep 3
done
