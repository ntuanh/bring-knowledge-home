/*
 * util.c - small helpers shared by every stage.
 *
 * Role: not a pipeline stage. Provides checked memory allocation, a growable
 * string buffer (StrBuf) and the diagnostics list.
 * Input/output: none of its own.
 * Called by: every other source file.
 *
 * Memory is never freed: the compiler runs once and exits, so the operating
 * system reclaims everything. This keeps the stage code easy to read.
 */
#include "minic.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) {
        fprintf(stderr, "minic: out of memory\n");
        exit(2);
    }
    return p;
}

void *xcalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "minic: out of memory\n");
        exit(2);
    }
    return p;
}

void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size ? size : 1);
    if (!p) {
        fprintf(stderr, "minic: out of memory\n");
        exit(2);
    }
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

static char *vformat(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *buf = xmalloc((size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    return buf;
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = vformat(fmt, ap);
    va_end(ap);
    return s;
}

void sb_init(StrBuf *sb)
{
    sb->cap = 64;
    sb->len = 0;
    sb->data = xmalloc(sb->cap);
    sb->data[0] = '\0';
}

static void sb_reserve(StrBuf *sb, size_t extra)
{
    if (sb->len + extra + 1 > sb->cap) {
        while (sb->len + extra + 1 > sb->cap)
            sb->cap *= 2;
        sb->data = xrealloc(sb->data, sb->cap);
    }
}

void sb_putc(StrBuf *sb, char c)
{
    sb_reserve(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s)
{
    size_t n = strlen(s);
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n + 1);
    sb->len += n;
}

void sb_printf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = vformat(fmt, ap);
    va_end(ap);
    sb_puts(sb, s);
    free(s);
}

void diag_add(DiagList *list, const char *stage, int line, int col, const char *fmt, ...)
{
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 8;
        list->items = xrealloc(list->items, (size_t)list->cap * sizeof(Diag));
    }
    va_list ap;
    va_start(ap, fmt);
    char *msg = vformat(fmt, ap);
    va_end(ap);
    list->items[list->count++] = (Diag){stage, line, col, msg};
}
