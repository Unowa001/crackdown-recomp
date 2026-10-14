#!/bin/sh
# Apply patches/sdk/*.patch (SDK fixes not upstream yet) to the sdk submodule,
# in name order. Patches already in (they reverse-apply) are skipped. After a
# patch is newly applied, rebuild + install the SDK and delete
# crackdown/generated/default/codegen*.stamp: the codegen stamp fingerprints
# only the SDK version string, which patching doesn't change.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
applied=0
for p in "$root"/patches/sdk/*.patch; do
  if git -C "$root/sdk" apply --unidiff-zero --reverse --check "$p" 2>/dev/null; then
    continue
  fi
  echo "Applying SDK patch $(basename "$p")"
  git -C "$root/sdk" apply --unidiff-zero "$p"
  applied=1
done
# The skip test above needs every patch to reverse-apply on its own with all
# of them in. A later patch whose hunk sits inside an earlier one's context
# breaks that, and the next run would re-apply (and fail on) the earlier one.
for p in "$root"/patches/sdk/*.patch; do
  if ! git -C "$root/sdk" apply --unidiff-zero --reverse --check "$p" 2>/dev/null; then
    echo "error: $(basename "$p") no longer reverse-applies with all patches in;" >&2
    echo "regenerate the later patch that edits next to it" >&2
    exit 1
  fi
done
if [ "$applied" = 1 ]; then
  rm -f "$root"/crackdown/generated/default/codegen.stamp \
        "$root"/crackdown/generated/default/codegen.build.stamp
  echo "Patched: rebuild + install the SDK, then delete the crackdown exe so it relinks and picks up the new SDK DLLs."
fi
