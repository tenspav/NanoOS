#!/bin/sh
# Host build+run of the bigload test under ASan/UBSan. Output in bigtest/build/.
set -e
cd "$(dirname "$0")"
mkdir -p build
cc -std=c99 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
   -o build/bigtest bigtest.c ../bigload.c
./build/bigtest
