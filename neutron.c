/*
 * neutron.c — Main driver, utilities, and C type system
 *
 * Usage:
 *   neutron [options] <input.c> -o <output.s>
 *
 * Options:
 *   -o <file>    Write assembly to <file> (default: stdout)
 *   -E           Preprocess only, print to stdout
 *   -dump-tokens Print token stream and exit
 *   -dump-ast    Print AST and exit
 *   -I <path>    Add include search path
 */

#include "neutron.h"
#include <errno.h>

/* ============================================================
 * Global compilation flags
 * ============================================================ */

bool mode_32bit        = false;
bool flag_freestanding = false;
bool flag_nostdlib     = false;

/* ============================================================
 * Global string literal table
 * ============================================================ */

StrEntry *str_table = NULL;
int       str_count = 0;
static int str_cap  = 0;

int str_intern(const char *data, int len) {
    /* Return existing entry if identical */
    for (int i = 0; i < str_count; i++) {
        if (str_table[i].len == len &&
            memcmp(str_table[i].data, data, len) == 0)
            return str_table[i].id;
    }
    if (str_count >= str_cap) {
        str_cap = str_cap ? str_cap * 2 : 16;
        str_table = xrealloc(str_table, str_cap * sizeof *str_table);
    }
    int id = str_count;
    str_table[id].data = xstrndup(data, len);
    str_table[id].len  = len;
    str_table[id].id   = id;
    str_count++;
    return id;
}

/* ============================================================
 * Predefined types
 * ============================================================ */

Type *ty_void,   *ty_bool;
Type *ty_char,   *ty_uchar;
Type *ty_short,  *ty_ushort;
Type *ty_int,    *ty_uint;
Type *ty_long,   *ty_ulong;
Type *ty_llong,  *ty_ullong;
Type *ty_float,  *ty_double, *ty_ldouble;

static Type *make_basic(TyKind k, int size, int align) {
    Type *t = xcalloc(1, sizeof *t);
    t->kind  = k;
    t->size  = size;
    t->align = align;
    return t;
}

static void init_types(void) {
    ty_void   = make_basic(TY_VOID,  0, 1);
    ty_bool   = make_basic(TY_BOOL,  1, 1);
    ty_char   = make_basic(TY_CHAR,  1, 1);
    ty_uchar  = make_basic(TY_UCHAR, 1, 1);
    ty_short  = make_basic(TY_SHORT,  2, 2);
    ty_ushort = make_basic(TY_USHORT, 2, 2);
    ty_int    = make_basic(TY_INT,  4, 4);
    ty_uint   = make_basic(TY_UINT, 4, 4);
    ty_float  = make_basic(TY_FLOAT,  4, 4);
    ty_double = make_basic(TY_DOUBLE, 8, 8);
    if (mode_32bit) {
        /* i386 ILP32 ABI */
        ty_long    = make_basic(TY_LONG,    4, 4);
        ty_ulong   = make_basic(TY_ULONG,   4, 4);
        ty_llong   = make_basic(TY_LLONG,   8, 4);  /* 8-byte, 4-aligned */
        ty_ullong  = make_basic(TY_ULLONG,  8, 4);
        ty_ldouble = make_basic(TY_LDOUBLE, 12, 4); /* x87 80-bit padded to 12 */
    } else {
        /* x86-64 LP64 ABI */
        ty_long    = make_basic(TY_LONG,    8, 8);
        ty_ulong   = make_basic(TY_ULONG,   8, 8);
        ty_llong   = make_basic(TY_LLONG,   8, 8);
        ty_ullong  = make_basic(TY_ULLONG,  8, 8);
        ty_ldouble = make_basic(TY_LDOUBLE, 16, 16);
    }
}

/* ============================================================
 * Type predicates
 * ============================================================ */

bool ty_is_integer(const Type *t) {
    switch (t->kind) {
    case TY_BOOL: case TY_CHAR:  case TY_UCHAR:
    case TY_SHORT: case TY_USHORT:
    case TY_INT:   case TY_UINT:
    case TY_LONG:  case TY_ULONG:
    case TY_LLONG: case TY_ULLONG:
    case TY_ENUM:
        return true;
    default:
        return false;
    }
}

bool ty_is_float(const Type *t) {
    return t->kind == TY_FLOAT || t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE;
}

bool ty_is_arithmetic(const Type *t) {
    return ty_is_integer(t) || ty_is_float(t);
}

bool ty_is_pointer(const Type *t) {
    return t->kind == TY_PTR || t->kind == TY_ARRAY;
}

bool ty_is_scalar(const Type *t) {
    return ty_is_arithmetic(t) || ty_is_pointer(t);
}

bool ty_is_signed(const Type *t) {
    switch (t->kind) {
    case TY_CHAR: case TY_SHORT: case TY_INT:
    case TY_LONG: case TY_LLONG:
        return true;
    default:
        return false;
    }
}

bool ty_is_void_ptr(const Type *t) {
    return t->kind == TY_PTR && t->base->kind == TY_VOID;
}

/* ============================================================
 * Type constructors
 * ============================================================ */

Type *ty_ptr_to(Type *base) {
    Type *t = xcalloc(1, sizeof *t);
    t->kind  = TY_PTR;
    t->size  = mode_32bit ? 4 : 8;
    t->align = mode_32bit ? 4 : 8;
    t->base  = base;
    return t;
}

Type *ty_array_of(Type *base, int len) {
    Type *t = xcalloc(1, sizeof *t);
    t->kind  = TY_ARRAY;
    t->base  = base;
    t->len   = len;
    t->size  = (len < 0) ? 0 : base->size * len;
    t->align = base->align;
    return t;
}

Type *ty_func(Type *ret, Param *params, bool variadic) {
    Type *t = xcalloc(1, sizeof *t);
    t->kind     = TY_FUNC;
    t->size     = 1;   /* function types have size 1 in GNU C */
    t->align    = 1;
    t->ret      = ret;
    t->params   = params;
    t->variadic = variadic;
    return t;
}

Type *ty_make_struct(char *tag) {
    Type *t = xcalloc(1, sizeof *t);
    t->kind = TY_STRUCT;
    t->tag  = tag;
    return t;
}

Type *ty_make_union(char *tag) {
    Type *t = xcalloc(1, sizeof *t);
    t->kind = TY_UNION;
    t->tag  = tag;
    return t;
}

/* Array / function decay to pointer */
Type *ty_decay(Type *t) {
    if (t->kind == TY_ARRAY)
        return ty_ptr_to(t->base);
    if (t->kind == TY_FUNC)
        return ty_ptr_to(t);
    return t;
}

Type *ty_clone(const Type *t) {
    Type *c = xmalloc(sizeof *c);
    *c = *t;
    return c;
}

/*
 * "Usual arithmetic conversions" (C99 §6.3.1.8).
 * Given two arithmetic types, returns the common type.
 */
Type *ty_usual_arith(Type *a, Type *b) {
    if (a->kind == TY_LDOUBLE || b->kind == TY_LDOUBLE) return ty_ldouble;
    if (a->kind == TY_DOUBLE  || b->kind == TY_DOUBLE)  return ty_double;
    if (a->kind == TY_FLOAT   || b->kind == TY_FLOAT)   return ty_float;

    /* Integer promotions first */
    if (a->size < 4) a = ty_int;
    if (b->size < 4) b = ty_int;

    if (a->size != b->size)
        return (a->size > b->size) ? a : b;

    /* Same size: prefer unsigned */
    return ty_is_signed(a) ? b : a;
}

/* ============================================================
 * AST node constructors
 * ============================================================ */

Node *new_node(NK kind, SrcLoc loc) {
    Node *n = xcalloc(1, sizeof *n);
    n->kind = kind;
    n->loc  = loc;
    return n;
}

Node *new_binary(NK kind, Node *l, Node *r, SrcLoc loc) {
    Node *n = new_node(kind, loc);
    n->left  = l;
    n->right = r;
    return n;
}

Node *new_unary(NK kind, Node *operand, SrcLoc loc) {
    Node *n = new_node(kind, loc);
    n->left = operand;
    return n;
}

Node *new_int_lit(long long val, SrcLoc loc) {
    Node *n = new_node(ND_INT_LIT, loc);
    n->ival = val;
    n->type = ty_int;
    return n;
}

Node *new_float_lit(long double val, SrcLoc loc) {
    Node *n = new_node(ND_FLOAT_LIT, loc);
    n->fval = val;
    n->type = ty_double;
    return n;
}

/* ============================================================
 * Error / warning
 * ============================================================ */

_Noreturn void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "neutron: fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

_Noreturn void error_at(SrcLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: error: ", loc.file, loc.line, loc.col);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

void warn_at(SrcLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: warning: ", loc.file, loc.line, loc.col);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ============================================================
 * Safe memory allocation
 * ============================================================ */

void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("out of memory (malloc %zu bytes)", n);
    return p;
}

void *xcalloc(size_t count, size_t size) {
    void *p = calloc(count, size);
    if (!p) die("out of memory (calloc %zu * %zu)", count, size);
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory (realloc %zu bytes)", n);
    return q;
}

char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) die("out of memory (strdup)");
    return p;
}

char *xstrndup(const char *s, size_t n) {
    char *p = strndup(s, n);
    if (!p) die("out of memory (strndup)");
    return p;
}

/* ============================================================
 * File I/O helper
 * ============================================================ */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open '%s': %s", path, strerror(errno ? errno : 0));

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    char *buf = xmalloc(sz + 2);
    if (fread(buf, 1, sz, f) != (size_t)sz)
        die("error reading '%s'", path);
    fclose(f);

    buf[sz]   = '\n';  /* ensure final newline */
    buf[sz+1] = '\0';
    return buf;
}

/* ============================================================
 * Include path list
 * ============================================================ */

#define MAX_INCLUDE_PATHS 64
const char *include_paths[MAX_INCLUDE_PATHS];
int n_include_paths = 0;

void add_include_path(const char *path) {
    if (n_include_paths >= MAX_INCLUDE_PATHS)
        die("too many -I paths");
    include_paths[n_include_paths++] = path;
}

/* ============================================================
 * main
 * ============================================================ */

static void usage(void) {
    fprintf(stderr,
        "Usage: neutron [options] <input.c> -o <output.s>\n"
        "Options:\n"
        "  -o <file>          Write assembly to <file> (default: stdout)\n"
        "  -m32               Target 32-bit i386 (cdecl ABI)\n"
        "  -ffreestanding     Freestanding environment (no hosted include paths)\n"
        "  -nostdlib          No standard library (implies -ffreestanding)\n"
        "  -O0/-O1/-O2/-O3    Optimization level (accepted, currently no-op)\n"
        "  -E                 Preprocess only\n"
        "  -dump-tokens       Dump token stream\n"
        "  -dump-ast          Dump AST\n"
        "  -I <path>          Add include search path\n"
    );
    exit(1);
}

int compiler_main(int argc, char **argv) {
    const char *input_file  = NULL;
    const char *output_file = NULL;
    bool flag_E           = false;
    bool flag_dump_tokens = false;
    bool flag_dump_ast    = false;

    /* First pass: pick up mode flags so init_types() uses correct sizes */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m32") == 0 || strcmp(argv[i], "-32b") == 0)
            mode_32bit = true;
        else if (strcmp(argv[i], "-ffreestanding") == 0)
            flag_freestanding = true;
        else if (strcmp(argv[i], "-nostdlib") == 0)
            flag_nostdlib = flag_freestanding = true;
    }

    init_types();

    /* Default hosted include paths — skipped in freestanding mode */
    if (!flag_freestanding) {
        add_include_path("/usr/include");
        add_include_path("/usr/local/include");
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) usage();
            output_file = argv[i];
        } else if (strcmp(argv[i], "-E") == 0) {
            flag_E = true;
        } else if (strcmp(argv[i], "-dump-tokens") == 0) {
            flag_dump_tokens = true;
        } else if (strcmp(argv[i], "-dump-ast") == 0) {
            flag_dump_ast = true;
        } else if (strcmp(argv[i], "-I") == 0) {
            if (++i >= argc) usage();
            add_include_path(argv[i]);
        } else if (strncmp(argv[i], "-I", 2) == 0) {
            add_include_path(argv[i] + 2);
        } else if (strcmp(argv[i], "-m32") == 0 || strcmp(argv[i], "-32b") == 0) {
            /* already handled in first pass */
        } else if (strcmp(argv[i], "-ffreestanding") == 0 ||
                   strcmp(argv[i], "-nostdlib") == 0) {
            /* already handled in first pass */
        } else if (strncmp(argv[i], "-O", 2) == 0) {
            /* optimization level — accepted, no-op for now */
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "neutron: unknown option '%s'\n", argv[i]);
            usage();
        } else {
            if (input_file)
                die("multiple input files not supported yet");
            input_file = argv[i];
        }
    }

    if (!input_file) usage();

    /* Read source */
    char *src = read_file(input_file);

    /* Preprocess */
    char *pp = preprocess(input_file, src);
    if (flag_E) {
        fputs(pp, stdout);
        return 0;
    }

    /* Lex */
    Token *toks = lex(input_file, pp);
    if (flag_dump_tokens) {
        static const char *tk_names[] = {
            "IDENT","INT_LIT","FLOAT_LIT","STR_LIT","CHAR_LIT",
            "auto","break","case","char","const","continue","default","do",
            "double","else","enum","extern","float","for","goto","if","inline",
            "int","long","register","restrict","return","short","signed",
            "sizeof","static","struct","switch","typedef","union","unsigned",
            "void","volatile","while","_Bool","_Alignof",
            "(",")","{","}","[","]",";",":",",",".","->","...","?","#",
            "=","+=","-=","*=","/=","%=","&=","|=","^=","<<=",">>=",
            "+","-","*","/","%","&","|","^","~","<<",">>",
            "!","&&","||","==","!=","<",">","<=",">=","++","--","EOF"
        };
        int n_names = (int)(sizeof tk_names / sizeof *tk_names);
        for (Token *t = toks; t->kind != TK_EOF; t = t->next) {
            const char *kname = (int)t->kind < n_names ? tk_names[t->kind] : "?";
            const char *val   = t->sval ? t->sval : "";
            if (t->kind == TK_INT_LIT || t->kind == TK_CHAR_LIT)
                printf("%s:%d:%d  %-12s  %lld\n",
                       t->loc.file, t->loc.line, t->loc.col, kname,
                       (long long)t->ival);
            else if (t->kind == TK_FLOAT_LIT)
                printf("%s:%d:%d  %-12s  %Lg\n",
                       t->loc.file, t->loc.line, t->loc.col, kname, t->fval);
            else
                printf("%s:%d:%d  %-12s  %s\n",
                       t->loc.file, t->loc.line, t->loc.col, kname, val);
        }
        return 0;
    }

    /* Parse */
    Node *prog = parse(toks);
    if (flag_dump_ast) {
        /* minimal dump */
        fprintf(stderr, "AST root kind=%d  n_stmts=%d\n",
                prog->kind, prog->n_stmts);
        return 0;
    }

    /* Semantic analysis */
    sema(prog);

    /* Code generation */
    FILE *out = stdout;
    if (output_file) {
        out = fopen(output_file, "w");
        if (!out) die("cannot open output '%s'", output_file);
    }
    codegen(prog, out);
    if (output_file) fclose(out);

    return 0;
}
