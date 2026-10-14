#!/usr/bin/env bash
# Recover functions reached only via pointers: register every unclaimed gap between
# generated functions, prune entries that break codegen, repeat until no new gaps.
# See docs/playbook.md "Gap scan". Usage: tools/gapscan.sh <project dir>  (e.g. crackdown)
set -euo pipefail
cd "$1"
REX=../sdk/out/install/linux-amd64/bin/rexglue
CFG=config/gapfill.toml
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
codegen() { "$REX" codegen *_manifest.toml > "$T/log" 2>&1; }

# start, instr count, last mnemonic per generated function
funcs() { cat generated/default/*_recomp.*.cpp | awk '
  /^DEFINE_REX_FUNC\(sub_/ { if (c!="") print c, n, l; c=substr($0,21,8); n=0; next }
  c!="" && /^\t\/\/ / && !/FATAL/ { n++; l=$2 }
  END { print c, n, l }' | sort; }

for round in 1 2 3 4 5 6; do
  codegen
  funcs > "$T/f"
  # gaps >= 8 bytes after a function that ends (blr/bctr/b) — after `bl` code may fall through
  awk '{s=strtonum("0x"$1); if (NR>1 && s-pe>=8 && (pl=="blr"||pl=="bctr"||pl=="b")) printf "0x%08X = {}  # gap %d bytes\n", pe, s-pe; pe=s+4*$2; pl=$3}' "$T/f" > "$T/new"
  # skip addresses already present (kept or previously rejected)
  touch "$T/rejected"
  grep -v -F -f <(grep -o '^0x[0-9A-F]*' "$CFG" "$T/rejected" 2>/dev/null | sed 's/.*:\(0x[0-9A-F]*\)/\1 =/') "$T/new" > "$T/add" || true
  [ -s "$T/add" ] || { echo "round $round: no new gaps"; break; }
  cat "$T/add" >> "$CFG"
  # prune until codegen reports no unresolved branches
  for _ in 1 2 3 4 5; do
    codegen
    grep -qi unresolved "$T/log" || break
    grep -oE '0x[0-9A-Fa-f]{8}' "$T/log" | sed 's/^0x//' | tr a-f A-F | sort -u > "$T/err"
    awk 'NR==FNR{e[NR]=strtonum("0x"$1); n=NR; next}
      /^0x/{a=strtonum($1); split($0,p,"gap "); sz=p[2]+0; for(k=1;k<=n;k++) if(e[k]>=a && e[k]<a+sz){print > "/dev/stderr"; next} print; next} {print}' \
      "$T/err" "$CFG" > "$T/cfg" 2>>"$T/rejected"
    mv "$T/cfg" "$CFG"
  done
  # entries that produced no body (e.g. the import-thunk table) break linking
  codegen
  grep -oh 'DEFINE_REX_FUNC(sub_[0-9A-F]*)' generated/default/*.cpp | sed 's/.*sub_/0x/;s/)//' | sort -u > "$T/def"
  grep -o '^0x[0-9A-F]*' "$CFG" | sort | comm -23 - "$T/def" > "$T/nobody" || true
  if [ -s "$T/nobody" ]; then grep -F -f <(sed 's/$/ =/' "$T/nobody") "$CFG" >> "$T/rejected"; grep -v -F -f <(sed 's/$/ =/' "$T/nobody") "$CFG" > "$T/cfg"; mv "$T/cfg" "$CFG"; fi
  echo "round $round: entries=$(grep -c '^0x' "$CFG") rejected=$(grep -c '^0x' "$T/rejected" || true)"
done
codegen
echo "unresolved: $(grep -ci unresolved "$T/log" || true)"
