/*
 * printf.c — minimal C runtime printf, puts, putchar
 * No libc dependency; uses the write(2) syscall directly.
 * Compile with: gcc -nostdlib -fno-builtin -Os -c
 */
#include <stdarg.h>
#include <stdint.h>

static long raw_write(int fd, const char *buf, long n) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"(1L), "D"((long)fd), "S"(buf), "d"(n)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static void emit_char(char c) {
    raw_write(1, &c, 1);
}

static void emit_str(const char *s) {
    if (!s) s = "(null)";
    long n = 0;
    while (s[n]) n++;
    raw_write(1, s, n);
}

static void emit_uint(unsigned long long v, int base, int upper) {
    char buf[24];
    const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (v == 0) { emit_char('0'); return; }
    while (v > 0) { buf[i++] = hex[v % base]; v /= base; }
    while (i--) emit_char(buf[i]);
}

static void emit_int(long long v, int base) {
    if (base == 10 && v < 0) { emit_char('-'); v = -v; }
    emit_uint((unsigned long long)v, base, 0);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { emit_char(*p); continue; }
        p++;
        int lng = 0;
        if (*p == 'l') { lng = 1; p++; }
        if (*p == 'l') { lng = 2; p++; }
        switch (*p) {
        case 'd': case 'i':
            emit_int(lng == 2 ? va_arg(ap, long long)
                   : lng == 1 ? va_arg(ap, long)
                   : va_arg(ap, int), 10);
            break;
        case 'u':
            emit_uint(lng == 2 ? va_arg(ap, unsigned long long)
                    : lng == 1 ? va_arg(ap, unsigned long)
                    : va_arg(ap, unsigned int), 10, 0);
            break;
        case 'x':
            emit_uint(lng == 2 ? va_arg(ap, unsigned long long)
                    : lng == 1 ? va_arg(ap, unsigned long)
                    : va_arg(ap, unsigned int), 16, 0);
            break;
        case 'X':
            emit_uint(lng == 2 ? va_arg(ap, unsigned long long)
                    : lng == 1 ? va_arg(ap, unsigned long)
                    : va_arg(ap, unsigned int), 16, 1);
            break;
        case 'p':
            emit_str("0x");
            emit_uint((unsigned long long)(uintptr_t)va_arg(ap, void*), 16, 0);
            break;
        case 's': emit_str(va_arg(ap, const char*)); break;
        case 'c': emit_char((char)va_arg(ap, int));  break;
        case '%': emit_char('%'); break;
        default:  emit_char('%'); emit_char(*p); break;
        }
    }
    va_end(ap);
    return 0;
}

int putchar(int c) {
    char ch = (char)c;
    raw_write(1, &ch, 1);
    return c;
}

int puts(const char *s) {
    emit_str(s);
    emit_char('\n');
    return 0;
}
