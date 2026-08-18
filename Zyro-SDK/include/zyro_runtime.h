#pragma once
#include <stddef.h>
#include <stdint.h>

// Zyro-SDK freestanding runtime helpers.

// A .zApp is compiled with -ffreestanding -nostdlib: there is no libc, no
// heap, no string.h, no printf. This header and zyro_runtime.cpp supplies the
// tiny subset most simple apps need, self-contained (no external
// symbols left for the linker to complain about. see pack_zapp.py,
// which refuses to produce a .zApp with any unresolved reference).

// You don't need to #include this directly for memcpy/memset/strlen/new.
// those are just always linked in and available as normal. This header is
// only for the extra convenience helpers (number-to-string, etc).

#ifdef __cplusplus
extern "C" {
#endif

// Formats a non-negative or negative integer into buf (decimal). Returns the
// number of characters written (excluding the NUL). bufSize must be at least
// 12 to be safe for any int32_t value.
int zyro_itoa(int value, char *buf, size_t bufSize);

// Formats an unsigned integer into buf (decimal).
int zyro_utoa(unsigned int value, char *buf, size_t bufSize);

// Appends src onto the end of dst (which must already be NUL-terminated),
// without overflowing dstSize. Returns the resulting length of dst.
size_t zyro_strcat(char *dst, size_t dstSize, const char *src);

#ifdef __cplusplus
}
#endif

// Tiny bump allocator backing operator new/delete (see zyro_runtime.cpp).
// There is no free(): memory is reclaimed only when the whole app is torn
// down and its RAM buffers are released by the firmware. This is enough for
// small UI-state objects a simple app allocates once at init(). it is NOT
// suitable for apps that allocate in a loop. If you need that, keep your own
// fixed-size static buffers instead of calling new repeatedly.
#define ZYRO_HEAP_SIZE (4 * 1024)
