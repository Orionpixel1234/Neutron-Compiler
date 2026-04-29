/*
 * printf_32.c — minimal printf for 32-bit Neutron programs
 * Uses Linux int 0x80 (SYS_write=4, ebx=fd, ecx=buf, edx=len)
 * No libc dependency.
 */

#include <stdarg.h>

static void sys_write(int fd, const char *buf, int len) {
    __asm__ volatile (
        "int $0x80"
        :
        : "a"(4), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
}

static void write_str(int fd, const char *s, int len) {
    if (len > 0) sys_write(fd, s, len);
}

static void write_char(int fd, char c) {
    sys_write(fd, &c, 1);
}

static void write_uint(int fd, unsigned long v, int base,
                        const char *digits, int min_width, char pad) {
    char buf[32];
    int  i = 32;
    if (v == 0) { buf[--i] = '0'; }
    else {
        while (v) {
            buf[--i] = digits[v % base];
            v /= base;
        }
    }
    int len = 32 - i;
    while (len < min_width) { write_char(fd, pad); min_width--; }
    write_str(fd, buf + i, len);
}

int vprintf(const char *fmt, va_list ap) {
    int fd = 1;
    int written = 0;
    const char *p = fmt;
    while (*p) {
        if (*p != '%') {
            write_char(fd, *p++);
            written++;
            continue;
        }
        p++; /* skip '%' */

        /* Width / padding */
        char pad = ' ';
        int  width = 0;
        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }

        /* Length modifier */
        int is_long = 0, is_longlong = 0;
        if (*p == 'l') { is_long = 1; p++; }
        if (*p == 'l') { is_longlong = 1; p++; }
        if (*p == 'z' || *p == 'h') p++;

        switch (*p++) {
        case 'd': case 'i': {
            long v;
            if (is_longlong)     v = (long)va_arg(ap, long long);
            else if (is_long)    v = va_arg(ap, long);
            else                 v = va_arg(ap, int);
            if (v < 0) { write_char(fd, '-'); written++; v = -v; }
            write_uint(fd, (unsigned long)v, 10, "0123456789", width, pad);
            written++;
            break;
        }
        case 'u': {
            unsigned long v;
            if (is_longlong)   v = (unsigned long)va_arg(ap, unsigned long long);
            else if (is_long)  v = va_arg(ap, unsigned long);
            else               v = va_arg(ap, unsigned int);
            write_uint(fd, v, 10, "0123456789", width, pad);
            written++;
            break;
        }
        case 'x': {
            unsigned long v;
            if (is_longlong)   v = (unsigned long)va_arg(ap, unsigned long long);
            else if (is_long)  v = va_arg(ap, unsigned long);
            else               v = va_arg(ap, unsigned int);
            write_uint(fd, v, 16, "0123456789abcdef", width, pad);
            written++;
            break;
        }
        case 'X': {
            unsigned long v;
            if (is_longlong)   v = (unsigned long)va_arg(ap, unsigned long long);
            else if (is_long)  v = va_arg(ap, unsigned long);
            else               v = va_arg(ap, unsigned int);
            write_uint(fd, v, 16, "0123456789ABCDEF", width, pad);
            written++;
            break;
        }
        case 'p': {
            unsigned long v = (unsigned long)va_arg(ap, void *);
            write_str(fd, "0x", 2);
            write_uint(fd, v, 16, "0123456789abcdef", width, pad);
            written++;
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0;
            while (s[len]) len++;
            write_str(fd, s, len);
            written += len;
            break;
        }
        case 'c':
            write_char(fd, (char)va_arg(ap, int));
            written++;
            break;
        case '%':
            write_char(fd, '%');
            written++;
            break;
        default:
            break;
        }
    }
    return written;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
    sys_write(1, "\n", 1);
    return len + 1;
}

int putchar(int c) {
    char ch = (char)c;
    sys_write(1, &ch, 1);
    return c;
}
