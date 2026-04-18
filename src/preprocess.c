/*
 * preprocess.c — C Preprocessor for Neutron
 *
 * Handles:
 *   #include "file" / <file>
 *   #define NAME value          (object-like macro)
 *   #define NAME(params) body   (function-like macro)
 *   #undef
 *   #if / #ifdef / #ifndef / #elif / #else / #endif
 *   #error
 *   #pragma once
 *   #line
 *   __FILE__, __LINE__, __DATE__, __TIME__  built-ins
 *
 * Strategy: single-pass text substitution producing a new source string,
 * with #line directives emitted so the lexer preserves original locations.
 */

#include "../neutron.h"
#include <time.h>
#include <errno.h>
#include <unistd.h>

/* ============================================================
 * Macro table
 * ============================================================ */

#define MACRO_CAP_INIT 64

typedef struct MacroParam {
    char              *name;
    struct MacroParam *next;
} MacroParam;

typedef struct Macro {
    char       *name;
    bool        func_like;   /* true if NAME(...) form */
    bool        variadic;    /* true if last param is ... */
    MacroParam *params;
    int         n_params;
    char       *body;        /* replacement text, as a string */
    struct Macro *next;      /* hash-chain */
} Macro;

#define MACRO_BUCKETS 256
static Macro *macro_table[MACRO_BUCKETS];

static unsigned macro_hash(const char *s) {
    unsigned h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h & (MACRO_BUCKETS - 1);
}

static Macro *macro_find(const char *name) {
    for (Macro *m = macro_table[macro_hash(name)]; m; m = m->next)
        if (strcmp(m->name, name) == 0)
            return m;
    return NULL;
}

static void macro_define(Macro *m) {
    unsigned h = macro_hash(m->name);
    /* remove old definition */
    Macro **p = &macro_table[h];
    while (*p) {
        if (strcmp((*p)->name, m->name) == 0) {
            *p = (*p)->next;
            break;
        }
        p = &(*p)->next;
    }
    m->next = macro_table[h];
    macro_table[h] = m;
}

static void macro_undef(const char *name) {
    unsigned h = macro_hash(name);
    Macro **p = &macro_table[h];
    while (*p) {
        if (strcmp((*p)->name, name) == 0) {
            *p = (*p)->next;
            return;
        }
        p = &(*p)->next;
    }
}

/* ============================================================
 * Once-included files
 * ============================================================ */

#define ONCE_CAP 256
static const char *once_files[ONCE_CAP];
static int         once_count = 0;

static bool is_once_file(const char *path) {
    for (int i = 0; i < once_count; i++)
        if (strcmp(once_files[i], path) == 0)
            return true;
    return false;
}

static void mark_once_file(const char *path) {
    if (once_count < ONCE_CAP)
        once_files[once_count++] = xstrdup(path);
}

/* ============================================================
 * Dynamic output buffer
 * ============================================================ */

typedef struct {
    char *buf;
    int   len, cap;
} Buf;

static void buf_init(Buf *b) {
    b->cap = 4096;
    b->buf = xmalloc(b->cap);
    b->len = 0;
}

static void buf_push(Buf *b, char c) {
    if (b->len >= b->cap) {
        b->cap *= 2;
        b->buf = xrealloc(b->buf, b->cap);
    }
    b->buf[b->len++] = c;
}

static void buf_puts(Buf *b, const char *s) {
    while (*s) buf_push(b, *s++);
}

static void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_puts(b, tmp);
}

static char *buf_finish(Buf *b) {
    buf_push(b, '\0');
    return b->buf;
}

/* ============================================================
 * Line-reading helpers
 * ============================================================ */

/* Skip to end of current line (leaving '\n' in place) */
static const char *skip_to_eol(const char *p) {
    while (*p && *p != '\n') p++;
    return p;
}

/* Skip horizontal whitespace */
static const char *skip_hws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Read an identifier into a freshly allocated string; advance *pp */
static char *read_ident(const char **pp) {
    const char *p = *pp;
    if (!isalpha((unsigned char)*p) && *p != '_')
        return NULL;
    const char *start = p;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    *pp = p;
    return xstrndup(start, p - start);
}

/* ============================================================
 * #if expression evaluator (integer constant expressions only)
 * ============================================================ */

static long long eval_if_expr(const char *expr, const char *file, int line);

static long long eval_primary(const char **p, const char *file, int line) {
    *p = skip_hws(*p);
    if (**p == '(') {
        (*p)++;
        long long v = eval_if_expr(*p, file, line);
        *p = skip_hws(*p);
        if (**p == ')') (*p)++;
        return v;
    }
    if (strncmp(*p, "defined", 7) == 0 &&
        !isalnum((unsigned char)(*p)[7]) && (*p)[7] != '_') {
        *p += 7;
        *p = skip_hws(*p);
        bool paren = **p == '(';
        if (paren) { (*p)++; *p = skip_hws(*p); }
        char *name = read_ident(p);
        if (paren) { *p = skip_hws(*p); if (**p == ')') (*p)++; }
        long long v = name && macro_find(name) ? 1 : 0;
        free(name);
        return v;
    }
    if (isdigit((unsigned char)**p)) {
        char *end;
        long long v = strtoll(*p, &end, 0);
        *p = end;
        return v;
    }
    /* Unknown identifier → 0 */
    char *name = read_ident(p);
    if (name) { free(name); return 0; }
    (*p)++;
    return 0;
}

static long long eval_unary(const char **p, const char *file, int line) {
    *p = skip_hws(*p);
    if (**p == '!') { (*p)++; return !eval_unary(p, file, line); }
    if (**p == '~') { (*p)++; return ~eval_unary(p, file, line); }
    if (**p == '-') { (*p)++; return -eval_unary(p, file, line); }
    if (**p == '+') { (*p)++; return  eval_unary(p, file, line); }
    return eval_primary(p, file, line);
}

static long long eval_mul(const char **p, const char *file, int line) {
    long long v = eval_unary(p, file, line);
    for (;;) {
        *p = skip_hws(*p);
        if (**p == '*')      { (*p)++; v *= eval_unary(p, file, line); }
        else if (**p == '/') { (*p)++; long long r = eval_unary(p, file, line);
                                       v = r ? v / r : 0; }
        else if (**p == '%') { (*p)++; long long r = eval_unary(p, file, line);
                                       v = r ? v % r : 0; }
        else break;
    }
    return v;
}

static long long eval_add(const char **p, const char *file, int line) {
    long long v = eval_mul(p, file, line);
    for (;;) {
        *p = skip_hws(*p);
        if      (**p == '+') { (*p)++; v += eval_mul(p, file, line); }
        else if (**p == '-') { (*p)++; v -= eval_mul(p, file, line); }
        else break;
    }
    return v;
}

static long long eval_shift(const char **p, const char *file, int line) {
    long long v = eval_add(p, file, line);
    for (;;) {
        *p = skip_hws(*p);
        if ((*p)[0]=='<' && (*p)[1]=='<') { *p+=2; v <<= eval_add(p, file, line); }
        else if ((*p)[0]=='>' && (*p)[1]=='>') { *p+=2; v >>= eval_add(p, file, line); }
        else break;
    }
    return v;
}

static long long eval_rel(const char **p, const char *file, int line) {
    long long v = eval_shift(p, file, line);
    for (;;) {
        *p = skip_hws(*p);
        if ((*p)[0]=='<' && (*p)[1]=='=') { *p+=2; v = v <= eval_shift(p, file, line); }
        else if ((*p)[0]=='>' && (*p)[1]=='=') { *p+=2; v = v >= eval_shift(p, file, line); }
        else if (**p == '<' && (*p)[1] != '<') { (*p)++; v = v < eval_shift(p, file, line); }
        else if (**p == '>' && (*p)[1] != '>') { (*p)++; v = v > eval_shift(p, file, line); }
        else break;
    }
    return v;
}

static long long eval_eq(const char **p, const char *file, int line) {
    long long v = eval_rel(p, file, line);
    for (;;) {
        *p = skip_hws(*p);
        if ((*p)[0]=='=' && (*p)[1]=='=') { *p+=2; v = v == eval_rel(p, file, line); }
        else if ((*p)[0]=='!' && (*p)[1]=='=') { *p+=2; v = v != eval_rel(p, file, line); }
        else break;
    }
    return v;
}

static long long eval_bitand(const char **p, const char *file, int line) {
    long long v = eval_eq(p, file, line);
    while (skip_hws(*p), **p == '&' && (*p)[1] != '&')
        { (*p)++; v &= eval_eq(p, file, line); }
    return v;
}

static long long eval_bitxor(const char **p, const char *file, int line) {
    long long v = eval_bitand(p, file, line);
    while (skip_hws(*p), **p == '^')
        { (*p)++; v ^= eval_bitand(p, file, line); }
    return v;
}

static long long eval_bitor(const char **p, const char *file, int line) {
    long long v = eval_bitxor(p, file, line);
    while (skip_hws(*p), **p == '|' && (*p)[1] != '|')
        { (*p)++; v |= eval_bitxor(p, file, line); }
    return v;
}

static long long eval_logand(const char **p, const char *file, int line) {
    long long v = eval_bitor(p, file, line);
    while (skip_hws(*p), (*p)[0]=='&' && (*p)[1]=='&')
        { *p+=2; v = eval_bitor(p, file, line) && v; }
    return v;
}

static long long eval_logor(const char **p, const char *file, int line) {
    long long v = eval_logand(p, file, line);
    while (skip_hws(*p), (*p)[0]=='|' && (*p)[1]=='|')
        { *p+=2; v = eval_logand(p, file, line) || v; }
    return v;
}

static long long eval_ternary(const char **p, const char *file, int line) {
    long long v = eval_logor(p, file, line);
    *p = skip_hws(*p);
    if (**p == '?') {
        (*p)++;
        long long t = eval_ternary(p, file, line);
        *p = skip_hws(*p);
        if (**p == ':') (*p)++;
        long long f = eval_ternary(p, file, line);
        return v ? t : f;
    }
    return v;
}

static long long eval_if_expr(const char *expr, const char *file, int line) {
    const char *p = expr;
    return eval_ternary(&p, file, line);
}

/* ============================================================
 * Macro expansion
 * ============================================================ */

/* Expand a single macro occurrence.
 * Returns newly allocated string with expansion, or NULL if not a macro. */
static char *expand_macro(const char *name, const char **pp);

static char *expand_text(const char *text) {
    Buf out;
    buf_init(&out);
    const char *p = text;
    while (*p) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            char *ident = xstrndup(start, p - start);
            char *exp = expand_macro(ident, &p);
            free(ident);
            if (exp) {
                buf_puts(&out, exp);
                free(exp);
            } else {
                /* not a macro, output as-is */
                p = start;
                while (isalnum((unsigned char)*p) || *p == '_')
                    buf_push(&out, *p++);
            }
        } else {
            buf_push(&out, *p++);
        }
    }
    return buf_finish(&out);
}

static char *expand_macro(const char *name, const char **pp) {
    Macro *m = macro_find(name);
    if (!m) return NULL;

    if (!m->func_like) {
        /* Object-like: just expand body (recursively) */
        return expand_text(m->body);
    }

    /* Function-like: read arguments */
    const char *p = skip_hws(*pp);
    if (*p != '(') return NULL;   /* not a call */
    p++;  /* skip '(' */

    /* Collect arguments */
    char *args[64];
    int   n_args = 0;
    Buf   arg;

    int depth = 0;
    while (*p && !(*p == ')' && depth == 0)) {
        if (*p == ',' && depth == 0) {
            buf_push(&arg, '\0');
            if (n_args < 64) args[n_args++] = arg.buf;
            buf_init(&arg);
            p++;
            continue;
        }
        if (*p == '(') depth++;
        if (*p == ')') depth--;
        if (n_args == 0) buf_init(&arg);
        buf_push(&arg, *p++);
    }
    if (*p == ')') p++;
    buf_push(&arg, '\0');
    if (arg.len > 1 || n_args > 0) {
        if (n_args < 64) args[n_args++] = arg.buf;
    }

    *pp = p;

    /* Substitute parameters in body */
    Buf out;
    buf_init(&out);
    const char *b = m->body;
    while (*b) {
        if (m->variadic && strncmp(b, "__VA_ARGS__", 11) == 0 &&
            !isalnum((unsigned char)b[11]) && b[11] != '_') {
            /* paste all extra args */
            for (int i = m->n_params; i < n_args; i++) {
                if (i > m->n_params) buf_puts(&out, ", ");
                buf_puts(&out, args[i]);
            }
            b += 11;
            continue;
        }
        if (isalpha((unsigned char)*b) || *b == '_') {
            const char *start = b;
            while (isalnum((unsigned char)*b) || *b == '_') b++;
            char *pname = xstrndup(start, b - start);
            /* Find matching parameter */
            MacroParam *pm = m->params;
            int idx = 0;
            bool found = false;
            while (pm) {
                if (strcmp(pm->name, pname) == 0) { found = true; break; }
                pm = pm->next; idx++;
            }
            if (found && idx < n_args) {
                char *expanded = expand_text(args[idx]);
                buf_puts(&out, expanded);
                free(expanded);
            } else {
                buf_puts(&out, pname);
            }
            free(pname);
        } else {
            buf_push(&out, *b++);
        }
    }

    char *result = expand_text(buf_finish(&out));
    for (int i = 0; i < n_args; i++) free(args[i]);
    return result;
}

/* ============================================================
 * Include file resolution
 * ============================================================ */

extern const char *include_paths[];
extern int n_include_paths;

static char *find_include(const char *filename, const char *current_file,
                           bool system) {
    char path[1024];

    if (!system) {
        /* Try relative to current file's directory first */
        const char *slash = strrchr(current_file, '/');
        if (slash) {
            int dir_len = (int)(slash - current_file);
            snprintf(path, sizeof path, "%.*s/%s", dir_len, current_file, filename);
            if (access(path, R_OK) == 0)
                return xstrdup(path);
        }
        /* Try current directory */
        snprintf(path, sizeof path, "./%s", filename);
        if (access(path, R_OK) == 0) return xstrdup(path);
    }

    /* Search include paths */
    for (int i = 0; i < n_include_paths; i++) {
        snprintf(path, sizeof path, "%s/%s", include_paths[i], filename);
        if (access(path, R_OK) == 0) return xstrdup(path);
    }
    return NULL;
}

/* ============================================================
 * Core preprocessor
 * ============================================================ */

static char *do_preprocess(const char *filename, const char *src, int depth);

/* Read file and preprocess it */
static char *include_file(const char *path, int depth) {
    if (depth > 64)
        die("include depth exceeded (recursive include?)");
    if (is_once_file(path))
        return xstrdup("");

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    char *src = xmalloc(sz + 2);
    fread(src, 1, sz, f);
    fclose(f);
    src[sz] = '\n'; src[sz+1] = '\0';

    char *result = do_preprocess(path, src, depth + 1);
    free(src);
    return result;
}

static char *do_preprocess(const char *filename, const char *src, int depth) {
    Buf out;
    buf_init(&out);

    const char *p   = src;
    int         line = 1;

    /* Emit initial #line directive */
    buf_printf(&out, "# %d \"%s\"\n", line, filename);

    /* Conditional stack */
    #define COND_STACK 64
    int  cond_stack[COND_STACK];  /* 1 = including, 0 = skipping, 2 = done */
    int  cond_depth = 0;
    auto bool cur_skipping();
    bool cur_skipping() {
        for (int i = 0; i < cond_depth; i++)
            if (cond_stack[i] != 1) return true;
        return false;
    }

    while (*p) {
        /* Track newlines */
        if (*p == '\n') {
            buf_push(&out, '\n');
            line++;
            p++;
            continue;
        }

        /* Strip line-continuation */
        if (*p == '\\' && p[1] == '\n') {
            line++;
            p += 2;
            continue;
        }

        /* Skip block comments */
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') { buf_push(&out, '\n'); line++; }
                p++;
            }
            if (*p) p += 2;
            buf_push(&out, ' ');
            continue;
        }

        /* Skip line comments */
        if (p[0] == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n') p++;
            continue;
        }

        /* Skip string / char literals (don't expand macros inside) */
        if (*p == '"' || *p == '\'') {
            if (cur_skipping()) { p++; continue; }
            char q = *p;
            buf_push(&out, *p++);
            while (*p && *p != q) {
                if (*p == '\\') { buf_push(&out, *p++); }
                if (*p) buf_push(&out, *p++);
            }
            if (*p) buf_push(&out, *p++);
            continue;
        }

        /* Preprocessor directive */
        if (*p == '#') {
            p++;
            p = skip_hws(p);

            if (*p == '\n' || *p == '\0') continue;  /* null directive */

            char *directive = read_ident(&p);
            if (!directive) {
                p = skip_to_eol(p);
                continue;
            }

            /* ---- #pragma ---- */
            if (strcmp(directive, "pragma") == 0) {
                p = skip_hws(p);
                if (strncmp(p, "once", 4) == 0) {
                    mark_once_file(filename);
                }
                p = skip_to_eol(p);
                free(directive);
                continue;
            }

            /* ---- #error ---- */
            if (strcmp(directive, "error") == 0) {
                if (!cur_skipping()) {
                    p = skip_hws(p);
                    const char *msg_start = p;
                    p = skip_to_eol(p);
                    fprintf(stderr, "%s:%d: #error %.*s\n",
                            filename, line, (int)(p - msg_start), msg_start);
                    exit(1);
                }
                p = skip_to_eol(p);
                free(directive);
                continue;
            }

            /* ---- #line ---- */
            if (strcmp(directive, "line") == 0) {
                if (!cur_skipping()) {
                    p = skip_hws(p);
                    line = (int)strtol(p, (char**)&p, 10) - 1;
                    buf_printf(&out, "# %d \"%s\"\n", line + 1, filename);
                }
                p = skip_to_eol(p);
                free(directive);
                continue;
            }

            /* ---- #undef ---- */
            if (strcmp(directive, "undef") == 0) {
                if (!cur_skipping()) {
                    p = skip_hws(p);
                    char *name = read_ident(&p);
                    if (name) { macro_undef(name); free(name); }
                }
                p = skip_to_eol(p);
                free(directive);
                continue;
            }

            /* ---- #define ---- */
            if (strcmp(directive, "define") == 0) {
                p = skip_hws(p);
                char *name = read_ident(&p);
                if (!name || cur_skipping()) {
                    free(name);
                    p = skip_to_eol(p);
                    free(directive);
                    continue;
                }

                Macro *m = xcalloc(1, sizeof *m);
                m->name = name;

                if (*p == '(') {
                    /* Function-like macro */
                    m->func_like = true;
                    p++;
                    while (*p && *p != ')') {
                        p = skip_hws(p);
                        if (*p == ')') break;
                        if (p[0] == '.' && p[1] == '.' && p[2] == '.') {
                            m->variadic = true;
                            p += 3;
                            break;
                        }
                        char *pname = read_ident(&p);
                        if (!pname) break;
                        MacroParam *pm = xcalloc(1, sizeof *pm);
                        pm->name = pname;
                        /* Append to param list */
                        MacroParam **tail = &m->params;
                        while (*tail) tail = &(*tail)->next;
                        *tail = pm;
                        m->n_params++;
                        p = skip_hws(p);
                        if (*p == ',') p++;
                    }
                    if (*p == ')') p++;
                }

                /* Read body until end of logical line */
                p = skip_hws(p);
                const char *body_start = p;
                while (*p && *p != '\n') {
                    if (*p == '\\' && p[1] == '\n') { p += 2; line++; continue; }
                    p++;
                }
                m->body = xstrndup(body_start, p - body_start);
                macro_define(m);
                free(directive);
                continue;
            }

            /* ---- #ifdef ---- */
            if (strcmp(directive, "ifdef") == 0) {
                p = skip_hws(p);
                char *name = read_ident(&p);
                int val = (!cur_skipping() && name && macro_find(name)) ? 1 : 0;
                if (cur_skipping()) val = 0;
                if (cond_depth < COND_STACK)
                    cond_stack[cond_depth++] = val ? 1 : 0;
                free(name);
                p = skip_to_eol(p);
                free(directive);
                /* emit #line for accurate location tracking */
                buf_printf(&out, "# %d \"%s\"\n", line + 1, filename);
                continue;
            }

            /* ---- #ifndef ---- */
            if (strcmp(directive, "ifndef") == 0) {
                p = skip_hws(p);
                char *name = read_ident(&p);
                int val = (!cur_skipping() && !(name && macro_find(name))) ? 1 : 0;
                if (cur_skipping()) val = 0;
                if (cond_depth < COND_STACK)
                    cond_stack[cond_depth++] = val ? 1 : 0;
                free(name);
                p = skip_to_eol(p);
                free(directive);
                buf_printf(&out, "# %d \"%s\"\n", line + 1, filename);
                continue;
            }

            /* ---- #if ---- */
            if (strcmp(directive, "if") == 0) {
                p = skip_hws(p);
                const char *expr_start = p;
                p = skip_to_eol(p);
                char *expr = xstrndup(expr_start, p - expr_start);
                long long val = (!cur_skipping()) ?
                    eval_if_expr(expr, filename, line) : 0;
                free(expr);
                if (cond_depth < COND_STACK)
                    cond_stack[cond_depth++] = val ? 1 : 0;
                free(directive);
                buf_printf(&out, "# %d \"%s\"\n", line + 1, filename);
                continue;
            }

            /* ---- #elif ---- */
            if (strcmp(directive, "elif") == 0) {
                if (cond_depth > 0) {
                    int *top = &cond_stack[cond_depth - 1];
                    if (*top == 1) {
                        *top = 2; /* already taken this branch */
                    } else if (*top == 0) {
                        /* evaluate condition */
                        p = skip_hws(p);
                        const char *expr_start = p;
                        p = skip_to_eol(p);
                        char *expr = xstrndup(expr_start, p - expr_start);
                        long long val = eval_if_expr(expr, filename, line);
                        free(expr);
                        *top = val ? 1 : 0;
                        free(directive);
                        buf_printf(&out, "# %d \"%s\"\n", line + 1, filename);
                        continue;
                    }
                }
                p = skip_to_eol(p);
                free(directive);
                buf_printf(&out, "# %d \"%s\"\n", line + 1, filename);
                continue;
            }

            /* ---- #else ---- */
            if (strcmp(directive, "else") == 0) {
                if (cond_depth > 0) {
                    int *top = &cond_stack[cond_depth - 1];
                    if (*top == 0)      *top = 1;
                    else if (*top == 1) *top = 2;
                }
                p = skip_to_eol(p);
                free(directive);
                buf_printf(&out, "# %d \"%s\"\n", line + 1, filename);
                continue;
            }

            /* ---- #endif ---- */
            if (strcmp(directive, "endif") == 0) {
                if (cond_depth > 0) cond_depth--;
                p = skip_to_eol(p);
                free(directive);
                buf_printf(&out, "# %d \"%s\"\n", line + 1, filename);
                continue;
            }

            /* ---- #include ---- */
            if (strcmp(directive, "include") == 0) {
                if (cur_skipping()) { p = skip_to_eol(p); free(directive); continue; }
                p = skip_hws(p);
                bool system = false;
                char inc_name[512];
                if (*p == '<') {
                    system = true; p++;
                    int i = 0;
                    while (*p && *p != '>') inc_name[i++] = *p++;
                    inc_name[i] = '\0';
                    if (*p == '>') p++;
                } else if (*p == '"') {
                    p++;
                    int i = 0;
                    while (*p && *p != '"') inc_name[i++] = *p++;
                    inc_name[i] = '\0';
                    if (*p == '"') p++;
                } else {
                    /* macro-expanded include — not supported, skip */
                    p = skip_to_eol(p);
                    free(directive);
                    continue;
                }

                char *inc_path = find_include(inc_name, filename, system);
                if (!inc_path) {
                    fprintf(stderr, "%s:%d: error: cannot find include '%s'\n",
                            filename, line, inc_name);
                    exit(1);
                }
                char *inc_text = include_file(inc_path, depth);
                if (inc_text) {
                    buf_puts(&out, inc_text);
                    free(inc_text);
                }
                free(inc_path);
                /* restore location */
                buf_printf(&out, "# %d \"%s\"\n", line + 1, filename);
                p = skip_to_eol(p);
                free(directive);
                continue;
            }

            /* Unknown directive — skip */
            p = skip_to_eol(p);
            free(directive);
            continue;
        }  /* end '#' handling */

        /* Outside directives: expand macros */
        if (cur_skipping()) {
            /* skip non-directive lines without emitting */
            while (*p && *p != '\n') p++;
            continue;
        }

        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            char *ident = xstrndup(start, p - start);

            /* Built-in macros */
            if (strcmp(ident, "__FILE__") == 0) {
                buf_printf(&out, "\"%s\"", filename);
                free(ident); continue;
            }
            if (strcmp(ident, "__LINE__") == 0) {
                buf_printf(&out, "%d", line);
                free(ident); continue;
            }

            Macro *m_check = macro_find(ident);
            char *exp = m_check ? expand_macro(ident, &p) : NULL;
            free(ident);
            if (exp) {
                buf_puts(&out, exp);
                free(exp);
            } else {
                /* Not a macro — emit the identifier text as-is */
                for (const char *c = start; c < p; c++)
                    buf_push(&out, *c);
            }
            continue;
        }

        buf_push(&out, *p++);
    }

    return buf_finish(&out);
}

/* ============================================================
 * Public entry point
 * ============================================================ */

char *preprocess(const char *filename, const char *src) {
    /* Define standard predefined macros */
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char date_str[32], time_str[32];
    strftime(date_str, sizeof date_str, "\"%b %d %Y\"", tm);
    strftime(time_str, sizeof time_str, "\"%H:%M:%S\"", tm);

    /* __DATE__ and __TIME__ */
    {
        Macro *m = xcalloc(1, sizeof *m);
        m->name = xstrdup("__DATE__");
        m->body = xstrdup(date_str);
        macro_define(m);
    }
    {
        Macro *m = xcalloc(1, sizeof *m);
        m->name = xstrdup("__TIME__");
        m->body = xstrdup(time_str);
        macro_define(m);
    }
    {
        Macro *m = xcalloc(1, sizeof *m);
        m->name = xstrdup("__STDC__");
        m->body = xstrdup("1");
        macro_define(m);
    }
    {
        Macro *m = xcalloc(1, sizeof *m);
        m->name = xstrdup("__STDC_VERSION__");
        m->body = xstrdup("199901L");
        macro_define(m);
    }
    {
        Macro *m = xcalloc(1, sizeof *m);
        m->name = xstrdup("__neutron__");
        m->body = xstrdup("1");
        macro_define(m);
    }

    return do_preprocess(filename, src, 0);
}
