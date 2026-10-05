/* The C library's memory functions for code compiled on the phone (route B, D-151): the phone has no SDK, and the
 * translated code needs only these. Declarations only; libSystem has them. Written for pspocket, not taken from an SDK. */
#ifndef PSPOCKET_SYSROOT_STRING_H
#define PSPOCKET_SYSROOT_STRING_H
#include <stddef.h>
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
#endif
