#!/bin/sh
# Linux Stage A, step 1: clean worktree of wip/m2 + SDK v0.10 with patches 0001-0009, built and installed.
set -ex
W=$HOME/worktrees/linux-m2
[ -d "$W" ] || git -C "$HOME/DecompProject" worktree add --detach "$W" wip/m2
cd "$W"
git submodule update --init --recursive sdk
git -C sdk rev-parse HEAD
sh tools/apply-sdk-patches.sh
cd sdk
cmake --preset linux-amd64 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DREXGLUE_BUILD_TESTS=OFF
cmake --build out/build/linux-amd64 --config Release --target install
echo SDK_STAGE_A_OK
