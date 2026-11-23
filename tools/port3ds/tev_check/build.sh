#!/bin/sh
# Builds the host TEV check (tev_check.c + the port's gx_tev.c) as build/tev_check/tev_check.
set -e
cd "$(dirname "$0")/../../.."
mkdir -p build/tev_check
gcc -std=gnu11 -O1 -Wall -include port3ds/include/port3ds_prefix.h -Iport3ds/include -Iport3ds/game -Iinclude \
    -Ilibs/dolsdk2004/include -Ilibs/musyx/include \
    tools/port3ds/tev_check/tev_check.c port3ds/game/gx/gx_tev.c port3ds/game/tev_override_ttyd.c -lm -o build/tev_check/tev_check
