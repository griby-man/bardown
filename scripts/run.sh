#!/usr/bin/env bash
# launch bardown against the extracted disc
#   scripts/run.sh            normal run
#   scripts/run.sh harvest    log missing indirect-call targets instead of dying on the first one
# extra args after the mode go straight to the binary
set -euo pipefail
root="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
bin="${BARDOWN_BIN:-$root/out/build/linux-amd64-release/bardown}"
game="${BARDOWN_GAME:-$root/dumps/game}"
logs="$root/logs"
export LD_LIBRARY_PATH="$root/third_party/rexglue-sdk/out/install/linux-amd64/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$logs"

mode="${1:-run}"
[[ $# -gt 0 ]] && shift
case "$mode" in
  run) ;;
  quick) mode=run
    # tap A through language select, legal screen and press start, then hand back the pad
    export BARDOWN_INPUT="${BARDOWN_INPUT:-$(seq 3 0.5 25 | sed 's/$/:a/' | paste -sd';')}" ;;
  match) mode=run
    # unattended: alternate A and Start every 1.5s for two minutes. language select and dialogs
    # want A, the title screen only takes Start, and in a match a stray Start pause is undone by A
    export BARDOWN_INPUT="${BARDOWN_INPUT:-$(for i in $(seq 0 89); do
      printf '%s:%s;' "$(awk "BEGIN{print 20+1.5*$i}")" "$([[ $((i % 2)) == 1 ]] && echo start || echo a)"
    done)}" ;;
  harvest) export REX_HARVEST_UNREGISTERED=1 ;;
  avdump) export REX_ABORT_ON_GUEST_AV=1 ;;
  *) echo "usage: $(basename "$0") [run|quick|match|harvest|avdump] [args...]"; exit 1 ;;
esac

# BARDOWN_PROFILE=test keeps automated runs out of the real saves and cache
extra=()
if [[ ${BARDOWN_PROFILE:-} == test ]]; then
  extra=(--user_data_root="$root/logs/profile-test/user" --cache_root="$root/logs/profile-test/cache" --audio_mute=true)
  mkdir -p "$root/logs/profile-test"
fi

log="$logs/$(date +%Y%m%d-%H%M%S)-$mode.log"
echo "log: $log"

# always on: every frame's time, once a second, in the log, for when it doesn't feel smooth
export REX_LOG_FPS=1
# BARDOWN_HEADLESS=1 runs inside headless gamescope: no window on the desktop. the game must not
# see our WAYLAND_DISPLAY or SDL connects to the real compositor and the window shows up anyway.
# use BARDOWN_SHOT_DIR for screenshots, grim can't see it
wrap=()
if [[ ${BARDOWN_HEADLESS:-} == 1 ]]; then
  unset WAYLAND_DISPLAY
  export SDL_VIDEO_DRIVER=x11
  wrap=(gamescope --backend headless -w 1280 -h 720 -W 1280 -H 720 --)
fi

# keyboard drives the pad by default (start = enter, a = space, b = backspace, left stick = wasd)
# fsi: the host render-target path leaves the 3d scene black. readback: equipment is composited
# into a render target and read back, without it pads/gloves/helmets come out black
exec "${wrap[@]}" "$bin" --game_data_root="$game" --gpu_plugin=xenos --mnk_mode=true \
  --render_target_path_vulkan=fsi --readback_resolve=auto --vulkan_readback_resolve=true \
  --log_file="$log" "${extra[@]}" "$@"
