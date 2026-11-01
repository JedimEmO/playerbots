#!/bin/sh
# Builds and runs the run lifecycle tests, without the server: the run's core and rules and a fake world.
# From the playerbots checkout's root, or with its path as the first argument. Needs a C++17 compiler, as in the
# build image: docker run --rm -v "$PWD:/src:ro" wow-playground/build:local sh /src/tests/run_lifecycle/run.sh /src
set -eu
ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
${CXX:-g++} -std=c++17 -O1 -Wall -Wextra -Werror -I "$ROOT/playerbot" \
    "$ROOT/tests/run_lifecycle/RunLifecycleTest.cpp" "$ROOT/playerbot/GuildRunCore.cpp" -o "$OUT/run_lifecycle_test"
${CXX:-g++} -std=c++17 -O1 -Wall -Wextra -Werror -I "$ROOT/playerbot" \
    "$ROOT/tests/run_lifecycle/RunGuardTest.cpp" -o "$OUT/run_guard_test"
"$OUT/run_guard_test"
"$OUT/run_lifecycle_test"
