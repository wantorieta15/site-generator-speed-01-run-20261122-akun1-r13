#!/bin/sh
# Build the freestanding fused site-generator binary.
#   gcc -O3 -nostdlib -static -o site main.c start.s
set -e
cd "$(dirname "$0")"
gcc -O3 -march=x86-64-v2 -fno-stack-protector -nostdlib -static -o site main.c start.s
ls -la site
