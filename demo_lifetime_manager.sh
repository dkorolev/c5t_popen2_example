#!/bin/bash

set -e

echo
echo "=== MAKE RELEASE ==="
echo

make

echo
echo "=== DEMO RELEASE ALL ==="
echo

./.current/demo_lifetime_manager && (echo "UNCOOPERATIVE SHOULD HAVE FAILED") || echo "UNCOOPERATIVE FAILED AS EXPECTED"

echo
echo "=== DEMO RELEASE COOPERATIVE ==="
echo

./.current/demo_lifetime_manager --uncooperative=false

echo
echo "=== MAKE DEBUG ==="
echo

make debug

echo
echo "=== DEMO DEBUG ALL ==="
echo

./.current_debug/demo_lifetime_manager && (echo "UNCOOPERATIVE SHOULD HAVE FAILED") || echo "UNCOOPERATIVE FAILED AS EXPECTED"

echo
echo "=== DEMO DEBUG COOPERATIVE ==="
echo

./.current_debug/demo_lifetime_manager --uncooperative=false

echo
echo "=== DONE ==="
echo
