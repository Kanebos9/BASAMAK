#!/bin/bash
# BASAMAK offline regression suite - run BEFORE EVERY RELEASE.
# Builds the tests/ console apps into build_tests/ (separate from the plugin build) and runs them.
# Exit 0 = all pass. Add new tests: drop tests/XTest.cpp + add to the CMake foreach + LIST below.
cd "$(dirname "$0")/.."
TESTS="PolyTest PRollTest MergeTest PresetTest DuckTest NudgeTest"
cmake -B build_tests -DBASAMAK_TESTS=ON > /dev/null || { echo "configure FAILED"; exit 1; }
FAIL=0
for T in $TESTS; do
    echo "=== $T ==="
    cmake --build build_tests --config Release --target "$T" > /tmp/basamak_test_build.log 2>&1 \
        || { echo "BUILD FAILED"; tail -5 /tmp/basamak_test_build.log; FAIL=1; continue; }
    BIN=$(find build_tests -name "$T" -type f -perm +111 2>/dev/null | head -1)
    if [ -z "$BIN" ]; then echo "binary not found"; FAIL=1; continue; fi
    "$BIN" || FAIL=1
done
echo
[ $FAIL -eq 0 ] && echo ">>> ALL TESTS PASS" || echo ">>> FAILURES - do not release!"
exit $FAIL
