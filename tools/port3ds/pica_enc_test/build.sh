#!/bin/sh
# Builds the host tests of the PICA encoders (port3ds/plat/pica_enc.c) and the register-file model
# (port3ds/plat/pica_model.c) as build/pica_enc_test/pica_enc_test and build/pica_enc_test/pica_model_test.
# Run: build/pica_enc_test/pica_enc_test <gpudump or gpuhang .bin> [...]
#      build/pica_enc_test/pica_model_test [<gpudump or gpuhang .bin> ...]
set -e
cd "$(dirname "$0")/../../.."
mkdir -p build/pica_enc_test
gcc -std=gnu11 -O1 -Wall -Wextra tools/port3ds/pica_enc_test/pica_enc_test.c port3ds/plat/pica_enc.c \
    -o build/pica_enc_test/pica_enc_test
gcc -std=gnu11 -O1 -Wall -Wextra tools/port3ds/pica_enc_test/pica_model_test.c port3ds/plat/pica_enc.c \
    port3ds/plat/pica_model.c -o build/pica_enc_test/pica_model_test
