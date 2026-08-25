#!/bin/sh
set -eu

test_bin="${TMPDIR:-/tmp}/esp583-dashboard-model-test"
c++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  test/test_dashboard_model.cpp -o "$test_bin"
"$test_bin"
echo "Dashboard model tests PASS"
