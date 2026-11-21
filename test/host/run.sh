#!/bin/sh
# Host test for src/trainer_math.h. No board, no PlatformIO, about one second.
set -e
cd "$(dirname "$0")"
mkdir -p build
g++ -std=c++17 -O1 -Wall -Wextra -o build/test_trainer_math test_trainer_math.cpp
./build/test_trainer_math
