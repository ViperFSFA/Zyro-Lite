#include "zyro_runtime.h"

// Freestanding runtime: everything a .zApp needs that would normally come
// from libc/libstdc++, reimplemented here so the final link has zero
// external symbols. pack_zapp.py always compiles this file into every app
// alongside your source. you never need to touch it.

extern "C" {

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int value, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)value;
    while (n--) *d++ = v;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
    }
    return 0;
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++)) {}
    return dst;
}

// Called if a virtual function is ever invoked (shouldn't happen in
// correct code, but the symbol must exist for the linker if you use
// virtual classes at all).
void __cxa_pure_virtual() { for (;;) {} }

// Guards for function-local statics. -fno-threadsafe-statics avoids the
// thread-safety machinery, but GCC still emits a plain (non-atomic) guard
// check. this is a single-threaded target so a simple flag is correct.
int __cxa_guard_acquire(uint64_t *g) { return !(*((uint8_t *)g)); }
void __cxa_guard_release(uint64_t *g) { *((uint8_t *)g) = 1; }
void __cxa_guard_abort(uint64_t *) {}

// operator delete needs a matching sized/unsized extern "C"-less pair below
// (kept out of extern "C" since these are C++-linkage operators).

}

// Bump allocator backing operator new/delete. No free(): see zyro_runtime.h.
// Lives in .bss (zero-initialized, doesn't take up file space in the .zApp).
static uint8_t gZyroHeap[ZYRO_HEAP_SIZE];
static size_t gZyroHeapUsed = 0;

static void *zyroAlloc(size_t n) {
    // 8-byte align every allocation. cheap and avoids misaligned-access
    // faults on any struct containing a double/uint64_t.
    size_t aligned = (n + 7) & ~((size_t)7);
    if (gZyroHeapUsed + aligned > ZYRO_HEAP_SIZE) return nullptr; // caller should check for null
    void *p = &gZyroHeap[gZyroHeapUsed];
    gZyroHeapUsed += aligned;
    return p;
}

void *operator new(size_t n) { return zyroAlloc(n); }
void *operator new[](size_t n) { return zyroAlloc(n); }
void operator delete(void *) noexcept {}
void operator delete[](void *) noexcept {}
void operator delete(void *, size_t) noexcept {}
void operator delete[](void *, size_t) noexcept {}

// Helpers

extern "C" int zyro_itoa(int value, char *buf, size_t bufSize) {
    if (bufSize == 0) return 0;
    unsigned int uv;
    size_t i = 0;
    bool neg = value < 0;
    if (neg) {
        if (bufSize < 2) { buf[0] = 0; return 0; }
        buf[i++] = '-';
        uv = (unsigned int)(-(long)value);
    } else {
        uv = (unsigned int)value;
    }
    int n = zyro_utoa(uv, buf + i, bufSize - i);
    return (int)i + n;
}

extern "C" int zyro_utoa(unsigned int value, char *buf, size_t bufSize) {
    if (bufSize == 0) return 0;
    char tmp[10]; // max digits for a 32-bit unsigned value
    int t = 0;
    if (value == 0) tmp[t++] = '0';
    while (value > 0 && t < (int)sizeof(tmp)) {
        tmp[t++] = char('0' + (value % 10));
        value /= 10;
    }
    size_t n = (size_t)t < bufSize - 1 ? (size_t)t : bufSize - 1;
    for (size_t i = 0; i < n; i++) buf[i] = tmp[t - 1 - i];
    buf[n] = 0;
    return (int)n;
}

extern "C" size_t zyro_strcat(char *dst, size_t dstSize, const char *src) {
    size_t len = strlen(dst);
    size_t i = 0;
    while (len + i + 1 < dstSize && src[i]) { dst[len + i] = src[i]; i++; }
    dst[len + i] = 0;
    return len + i;
}
