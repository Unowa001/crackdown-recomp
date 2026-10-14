#!/bin/sh
# Print the PPC disassembly comments of one recompiled function: asm.sh sub_XXXXXXXX
f=$1; d=$(dirname "$0")/../../crackdown/generated/default
file=$(grep -l "^DEFINE_REX_FUNC($f)" "$d"/crackdown_recomp.*.cpp | head -1)
awk -v f="DEFINE_REX_FUNC($f) {" '$0==f{p=1;next} p&&/^DEFINE_REX_FUNC/{exit} p&&/^\t*\/\/ /{sub(/^\t*\/\/ /,"");print} p&&/^loc_/{print}' "$file"
