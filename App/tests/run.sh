#!/bin/sh
# Host build + run of DJCore tests (Linux or macOS).
set -e
cd "$(dirname "$0")"
SRC=../GRV6DJ.swiftpm/Sources/DJCore
c++ -std=c++17 -O2 -D_GLIBCXX_ASSERTIONS -Wall -Wextra -Wno-unused-parameter -I$SRC/include -I$SRC \
  $SRC/engine.cpp $SRC/analysis.cpp $SRC/ioproc.cpp $SRC/midi.cpp $SRC/rblib.cpp -DFIXTURES=\"$(pwd)/fixtures\" core_tests.cpp -o /tmp/djcore_tests
/tmp/djcore_tests
