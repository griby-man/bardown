#!/usr/bin/env bash
# screenshot the running bardown window every N seconds (matched by pid, never by title)
#   tools/shots.sh <outdir> <interval> <count>
set -euo pipefail
out="$1"; every="$2"; count="$3"
mkdir -p "$out"
for ((i = 0; i < count; i++)); do
  sleep "$every"
  pid=$(pgrep -nx bardown) || continue
  geom=$(hyprctl clients -j | python3 -c "
import json, sys
for c in json.load(sys.stdin):
    if c['pid'] == $pid:
        print('%d,%d %dx%d' % (*c['at'], *c['size'])); break")
  [[ -n $geom ]] && grim -g "$geom" "$out/$(printf '%03d' $(( (i + 1) * every ))).png"
done
