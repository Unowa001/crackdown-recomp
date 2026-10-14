#!/bin/sh
# Linux Stage A, step 2: codegen + Release game build + AP host tests in ~/worktrees/linux-m2.
set -ex
W=$HOME/worktrees/linux-m2
cd "$W"
[ -e game ] || ln -s "$HOME/DecompProject/game" game
cd crackdown
../sdk/out/install/linux-amd64/bin/rexglue codegen crackdown_manifest.toml
cmake --preset linux-amd64-release
cmake --build --preset linux-amd64-release
ls -la out/build/linux-amd64-release/crackdown* out/build/linux-amd64-release/*.so 2>/dev/null || true
cmake -S src/ap -B out/build/ap-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DSDK_THIRDPARTY=$W/sdk/thirdparty
cmake --build out/build/ap-tests
ctest --test-dir out/build/ap-tests --output-on-failure
echo GAME_STAGE_A_OK
