#ifndef _FDLIBM_H
#define _FDLIBM_H
/* little-endian host version of libs/msl fdlibm.h */
#include <math.h>
#define __HI(x) *(1 + (int*)&x)
#define __LO(x) *(int*)&x
int __ieee754_rem_pio2(double, double*);
double __kernel_sin(double, double, int);
double __kernel_cos(double, double);
int __kernel_rem_pio2(double*, double*, int, int, int, const int*);
#endif
