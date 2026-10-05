/* The C library's maths for code compiled on the phone (route B, D-151): what the translated code calls, declared;
 * libSystem has them. The classification macros go to the compiler's builtins. Written for pspocket. */
#ifndef PSPOCKET_SYSROOT_MATH_H
#define PSPOCKET_SYSROOT_MATH_H
double sqrt(double x);
float sqrtf(float x);
double fma(double x, double y, double z);
float fmaf(float x, float y, float z);
double fabs(double x);
float fabsf(float x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double rint(double x);
double nearbyint(double x);
float exp2f(float x);
float log2f(float x);
double copysign(double x, double y);
#define NAN __builtin_nanf("")
#define INFINITY __builtin_inff()
#define HUGE_VAL __builtin_huge_val()
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define FP_NAN 1
#define FP_INFINITE 2
#define FP_ZERO 3
#define FP_NORMAL 4
#define FP_SUBNORMAL 5
#endif
