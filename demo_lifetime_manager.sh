#!/bin/bash

set -e

echo
echo "=== MAKE RELEASE ==="
echo

echo ::group::{make}
make
echo ::endgroup::

echo
echo "=== DEMO RELEASE ALL ==="
echo

echo ::group::{release all}
./.current/demo_lifetime_manager && (echo "UNCOOPERATIVE SHOULD HAVE FAILED") || echo "UNCOOPERATIVE FAILED AS EXPECTED"
echo ::endgroup::

echo
echo "=== DEMO RELEASE COOPERATIVE ==="
echo

echo ::group::{release only cooperative}
./.current/demo_lifetime_manager --uncooperative=false
echo ::endgroup::

echo
echo "=== RELEASE CRASH TESTS ==="
echo

echo ::group::{release crash tests}
for i in ./.current/crashtest_* ; do echo "Running $i"; $i; done
echo ::endgroup::

echo
echo "=== MAKE DEBUG ==="
echo

echo ::group::{make debug}
make debug
echo ::endgroup::

echo
echo "=== DEMO DEBUG ALL ==="
echo

echo ::group::{debug all}
./.current_debug/demo_lifetime_manager && (echo "UNCOOPERATIVE SHOULD HAVE FAILED") || echo "UNCOOPERATIVE FAILED AS EXPECTED"
echo ::endgroup::

echo
echo "=== DEMO DEBUG COOPERATIVE ==="
echo

echo ::group::{debug only cooperative}
./.current_debug/demo_lifetime_manager --uncooperative=false
echo ::endgroup::

echo
echo "=== DEBUG CRASH TESTS ==="
echo

echo ::group::{debug crash tests}
for i in ./.current_debug/crashtest_* ; do echo "Running $i"; $i; done
echo ::endgroup::

echo
echo "=== DONE ==="
echo
