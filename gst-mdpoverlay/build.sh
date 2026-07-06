#!/bin/sh
CC=/home/herrie/x-tools/arm-unknown-linux-gnueabi-gcc125/bin/arm-unknown-linux-gnueabi-gcc
$CC -O2 -Wall -Wextra -static -o mdp_overlay_test mdp_overlay_test.c && echo "built $(ls -l mdp_overlay_test|awk '{print $5}')"
