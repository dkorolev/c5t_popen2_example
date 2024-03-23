#!/bin/bash

make && echo && ./.current/popen2/popen2_test --gtest_repeat=100

echo

make debug && echo && ./.current_debug/popen2/popen2_test --gtest_repeat=100
