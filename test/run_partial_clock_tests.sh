#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary="${TMPDIR:-/tmp}/partial_clock_model_test"

c++ -std=c++17 -Wall -Wextra -pedantic \
  "$project_root/test/test_partial_clock_model.cpp" \
  -o "$test_binary"

"$test_binary"
