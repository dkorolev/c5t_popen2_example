#!/bin/bash

N=${1:-1}

echo
echo "=== TEST ==="
echo

make && echo && ./.current/popen2/popen2_test --gtest_repeat=$N
echo
make debug && echo && ./.current_debug/popen2/popen2_test --gtest_repeat=$N

echo
echo "=== MODULE ==="
echo

(cd popen2; git rev-parse HEAD)

echo
echo "=== RUN ==="
echo

make && echo && ./.current/example_popen2
echo
make debug && echo && ./.current_debug/example_popen2
