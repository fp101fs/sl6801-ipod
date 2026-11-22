/* C library routines the compiler may call implicitly (struct copies, zero init), forwarded to the
 * boot ROM's versions. Unused ones are dropped by --gc-sections. */
#include "fw.h"

void *memset(void *dst, int c, size_t n)
{
	return rom_memset(dst, c, n);
}

void *memcpy(void *dst, const void *src, size_t n)
{
	return rom_memcpy(dst, src, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
	return rom_memmove(dst, src, n);
}

size_t strlen(const char *s)
{
	return rom_strlen(s);
}
