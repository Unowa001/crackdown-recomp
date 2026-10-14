#!/bin/sh
# Backtrace the first Linux "Unhandled guest access violation" (read of guest 0x706A0008).
W=$HOME/worktrees/linux-m2; B=$W/crackdown/out/build/linux-amd64-release
OUT=$HOME/linux-smoke/gdb; rm -rf $OUT; mkdir -p $OUT/user $OUT/cache; cp -a $HOME/linux-profiles/save1-user-data/. $OUT/user/
Xvfb :98 -screen 0 1280x720x24 > /dev/null 2>&1 & XP=$!; sleep 2
export DISPLAY=:98 SDL_VIDEODRIVER=x11 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json LD_LIBRARY_PATH=$W/sdk/out/install/linux-amd64/lib
cd $B
timeout -k 10 240 gdb -q -batch -x $(dirname "$0")/gdb_av.cmd --args ./crackdown --game_data_root=$W/game --user_data_root=$OUT/user --cache_root=$OUT/cache --input_replay=$HOME/linux-bench/stairs_tp.rec --teleport=1396.6,19.8,-1413.1 > $OUT/gdb.txt 2>&1
kill $XP
grep -v "New Thread\|exited\]" $OUT/gdb.txt | grep -E "^#|Breakpoint|breakpoint|Catchpoint|Function|rip" | cut -c1-260 | head -45
