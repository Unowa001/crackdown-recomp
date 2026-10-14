#!/bin/sh
# Linux boot smoke on the headless server: Xvfb + chosen Vulkan ICD, isolated user data, screenshots.
# usage: [TEMPLATE=<user data dir to copy>] [TAG=name] [CACHE=<persistent cache dir>] linux_smoke.sh <lvp|nvidia> [seconds] [extra game args...]
ICD=$1; T=${2:-150}; shift 2 2>/dev/null || shift $#
W=$HOME/worktrees/linux-m2; B=$W/crackdown/out/build/linux-amd64-release
OUT=$HOME/linux-smoke/${TAG:-$ICD}; rm -rf "$OUT"; mkdir -p "$OUT/user" "$OUT/cache"
[ -n "$TEMPLATE" ] && cp -a "$TEMPLATE/." "$OUT/user/"
LOGS=$W/crackdown/out/build/linux-amd64-release/logs; mkdir -p "$LOGS"; BEFORE=$(ls "$LOGS" | wc -l)
Xvfb :99 -screen 0 1280x720x24 > "$OUT/xvfb.log" 2>&1 & XPID=$!
sleep 2
export DISPLAY=:99 SDL_VIDEODRIVER=x11 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/${ICD}_icd.json
export LD_LIBRARY_PATH=$W/sdk/out/install/linux-amd64/lib
cd "$B"
timeout -s INT -k 15 "$T" ./crackdown --game_data_root="$W/game" --user_data_root="$OUT/user" --cache_root="${CACHE:-$OUT/cache}" "$@" > "$OUT/stdout.log" 2>&1 &
GP=$!
s=0
while kill -0 $GP 2>/dev/null && [ $s -lt "$T" ]; do
  sleep 30; s=$((s+30))
  xwd -root -display :99 -silent > "$OUT/shot.xwd" 2>/dev/null && convert "$OUT/shot.xwd" "$OUT/shot_${s}s.png" 2>/dev/null
done
wait $GP; echo "game exit=$?" >> "$OUT/stdout.log"
kill $XPID 2>/dev/null
rm -f "$OUT/shot.xwd"
L=$(ls -t "$LOGS"/crackdown_*.log 2>/dev/null | head -1); [ -n "$L" ] && cp "$L" "$OUT/game.log"
ls "$OUT"; tail -5 "$OUT/stdout.log"
echo SMOKE_DONE
