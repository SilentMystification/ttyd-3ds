#!/bin/sh
# Host study: devkitARM newlib sinf (transliterated from libm_a-sinf.o, tables from libm_a-sincosf_data.o)
# against (float)fdlibm sin((double)x) from libs/msl. Run from the repo root.
#   ./run.sh        build into /tmp/sinstudy
#   /tmp/sinstudy/study 0   exhaustive, all 2^32 floats (OpenMP)
#   /tmp/sinstudy/study 1   timing   /study 2  sanity vs host sinf   /study 3  correct-rounding sample
set -e
W=/tmp/sinstudy; mkdir -p $W
M=libs/msl/src/MSL_C.PPCEABI.bare.H.a
for f in s_sin k_sin k_cos e_rem_pio2 k_rem_pio2; do cp $M/$f.c $W/; done
sed -i 's/^double sin(double x)/double msl_sin(double x)/' $W/s_sin.c
cp tools/port3ds/sinstudy/fdlibm.h tools/port3ds/sinstudy/tables.h tools/port3ds/sinstudy/study.c $W/
cd $W && gcc -O2 -fopenmp -ffp-contract=off -w -I. study.c s_sin.c k_sin.c k_cos.c e_rem_pio2.c k_rem_pio2.c -lm -o study
