/*
 * neutron.h — Core types and declarations for the Neutron C Compiler
 *
 * Pipeline:  source → [preprocess] → [lex] → [parse] → [sema] → [codegen] → .s
 * Target:    x86-64 Linux (System V AMD64 ABI)
 * Language:  C99 subset (no VLAs, no complex, no _Generic)
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <assert.h>

/* ============================================================
 * Source location
 * ============================================================ */

typedef struct {
    const char *file;
    int line, col;
} SrcLoc;

/* ============================================================
 * Token kinds
 * ============================================================ */

typedef enum {
    /* Literals */
    TK_IDENT,
    TK_INT_LIT,
    TK_FLOAT_LIT,
    TK_STR_LIT,
    TK_CHAR_LIT,

    /* Keywords — must stay in this order; see keyword table in lex.c */
    TK_AUTO, TK_BREAK, TK_CASE, TK_CHAR, TK_CONST,
    TK_CONTINUE, TK_DEFAULT, TK_DO, TK_DOUBLE, TK_ELSE,
    TK_ENUM, TK_EXTERN, TK_FLOAT, TK_FOR, TK_GOTO,
    TK_IF, TK_INLINE, TK_INT, TK_LONG, TK_REGISTER,
    TK_RESTRICT, TK_RETURN, TK_SHORT, TK_SIGNED, TK_SIZEOF,
    TK_STATIC, TK_STRUCT, TK_SWITCH, TK_TYPEDEF, TK_UNION,
    TK_UNSIGNED, TK_VOID, TK_VOLATILE, TK_WHILE,
    TK__BOOL, TK__ALIGNOF,

    /* Punctuators */
    TK_LPAREN, TK_RPAREN,      /* ( )   */
    TK_LBRACE, TK_RBRACE,      /* { }   */
    TK_LBRACKET, TK_RBRACKET,  /* [ ]   */
    TK_SEMI,                   /* ;     */
    TK_COLON,                  /* :     */
    TK_COMMA,                  /* ,     */
    TK_DOT,                    /* .     */
    TK_ARROW,                  /* ->    */
    TK_ELLIPSIS,               /* ...   */
    TK_QUESTION,               /* ?     */
    TK_HASH,                   /* #     */

    /* Assignment operators */
    TK_ASSIGN,                 /* =     */
    TK_PLUS_EQ,                /* +=    */
    TK_MINUS_EQ,               /* -=    */
    TK_STAR_EQ,                /* *=    */
    TK_SLASH_EQ,               /* /=    */
    TK_PERCENT_EQ,             /* %=    */
    TK_AMP_EQ,                 /* &=    */
    TK_PIPE_EQ,                /* |=    */
    TK_CARET_EQ,               /* ^=    */
    TK_LSHIFT_EQ,              /* <<=   */
    TK_RSHIFT_EQ,              /* >>=   */

    /* Arithmetic */
    TK_PLUS, TK_MINUS,
    TK_STAR, TK_SLASH, TK_PERCENT,

    /* Bitwise */
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE,
    TK_LSHIFT, TK_RSHIFT,

    /* Logical / relational */
    TK_BANG,
    TK_AND, TK_OR,
    TK_EQ, TK_NEQ,
    TK_LT, TK_GT, TK_LEQ, TK_GEQ,

    /* Increment / decrement */
    TK_INC, TK_DEC,

    TK_EOF,
} TK;

/* ---------------------------------------------------------------- */

typedef struct Token Token;
struct Token {
    TK      kind;
    SrcLoc  loc;

    /* TK_INT_LIT / TK_CHAR_LIT */
    unsigned long long ival;
    bool is_unsigned;
    bool is_long;
    bool is_llong;

    /* TK_FLOAT_LIT */
    long double fval;
    bool is_float;   /* true = float suffix, false = double */

    /* TK_STR_LIT, TK_IDENT */
    char *sval;      /* null-terminated copy */
    int   slen;      /* byte count excl. terminating null */

    Token *next;
};

/* ============================================================
 * C type system
 * ============================================================ */

typedef struct Type   Type;
typedef struct Member Member;
typedef struct Param  Param;

typedef enum {
    TY_VOID,
    TY_BOOL,
    TY_CHAR,   TY_UCHAR,
    TY_SHORT,  TY_USHORT,
    TY_INT,    TY_UINT,
    TY_LONG,   TY_ULONG,
    TY_LLONG,  TY_ULLONG,
    TY_FLOAT,
    TY_DOUBLE,
    TY_LDOUBLE,
    TY_PTR,
    TY_ARRAY,
    TY_STRUCT,
    TY_UNION,
    TY_FUNC,
    TY_ENUM,
} TyKind;

struct Param {
    char  *name;
    Type  *type;
    Param *next;
};

struct Member {
    char   *name;
    Type   *type;
    int     offset;
    Member *next;
};

struct Type {
    TyKind kind;
    int    size;       /* sizeof value in bytes              */
    int    align;      /* alignment in bytes                 */
    bool   is_const;
    bool   is_volatile;
    bool   is_restrict;

    /* PTR / ARRAY */
    Type  *base;

    /* ARRAY */
    int    len;        /* number of elements; -1 = incomplete */

    /* FUNC */
    Type  *ret;
    Param *params;
    bool   variadic;

    /* STRUCT / UNION */
    char   *tag;
    Member *members;
    bool    complete;   /* false until closing } is parsed   */
};

/* Predefined scalar types — allocated once in neutron.c */
extern Type *ty_void,   *ty_bool;
extern Type *ty_char,   *ty_uchar;
extern Type *ty_short,  *ty_ushort;
extern Type *ty_int,    *ty_uint;
extern Type *ty_long,   *ty_ulong;
extern Type *ty_llong,  *ty_ullong;
extern Type *ty_float,  *ty_double, *ty_ldouble;

bool  ty_is_integer   (const Type *t);
bool  ty_is_float     (const Type *t);
bool  ty_is_arithmetic(const Type *t);
bool  ty_is_pointer   (const Type *t);
bool  ty_is_scalar    (const Type *t);
bool  ty_is_signed    (const Type *t);
bool  ty_is_void_ptr  (const Type *t);
Type *ty_ptr_to       (Type *base);
Type *ty_array_of     (Type *base, int len);
Type *ty_func         (Type *ret, Param *params, bool variadic);
Type *ty_make_struct  (char *tag);
Type *ty_make_union   (char *tag);
Type *ty_decay        (Type *t);     /* array/function → pointer */
Type *ty_usual_arith  (Type *a, Type *b);
Type *ty_clone        (const Type *t);

/* ============================================================
 * Symbol (one entry in a scope's variable / tag list)
 * ============================================================ */

typedef struct Symbol Symbol;
struct Symbol {
    char   *name;
    Type   *type;

    bool    is_global;
    bool    is_static;
    bool    is_extern;
    bool    is_typedef;   /* typedef name, not a real variable */
    bool    is_defined;

    int     offset;       /* local variable: negative offset from %rbp */
    char   *asm_name;     /* global: label string used in output asm    */

    long long enum_val;   /* for enum constants                          */

    Symbol *next;         /* next symbol in the same scope               */
};

typedef struct Scope Scope;
struct Scope {
    Symbol *vars;    /* variables, typedefs, enum constants */
    Symbol *tags;    /* struct / union / enum tags           */
    Scope  *parent;
};

/* ============================================================
 * AST node kinds
 * ============================================================ */

typedef enum {
    /* Translation unit */
    ND_PROG,

    /* Top-level / block-level declarations */
    ND_FUNC_DEF,
    ND_VAR_DECL,

    /* Statements */
    ND_BLOCK,
    ND_EXPR_STMT,
    ND_IF,
    ND_SWITCH,
    ND_WHILE,
    ND_DO_WHILE,
    ND_FOR,
    ND_RETURN,
    ND_BREAK,
    ND_CONTINUE,
    ND_GOTO,
    ND_LABEL,
    ND_CASE,
    ND_DEFAULT,
    ND_NULL_STMT,

    /* Expressions (in rough precedence order, low to high) */
    ND_COMMA,
    ND_ASSIGN,    /* = */
    ND_ASSIGN_OP, /* +=, -=, … — op field holds the TK_* operator */
    ND_TERNARY,   /* ?: */
    ND_LOGOR,     /* || */
    ND_LOGAND,    /* && */
    ND_BITOR,     /* |  */
    ND_BITXOR,    /* ^  */
    ND_BITAND,    /* &  */
    ND_EQ,  ND_NEQ,
    ND_LT,  ND_GT,  ND_LEQ,  ND_GEQ,
    ND_SHL, ND_SHR,
    ND_ADD, ND_SUB,
    ND_MUL, ND_DIV, ND_MOD,
    ND_CAST,
    ND_PRE_INC,  ND_PRE_DEC,
    ND_POST_INC, ND_POST_DEC,
    ND_ADDR,    /* &x   */
    ND_DEREF,   /* *x   */
    ND_NOT,     /* !x   */
    ND_BITNOT,  /* ~x   */
    ND_NEG,     /* -x   */
    ND_SIZEOF,
    ND_ALIGNOF,
    ND_CALL,
    ND_INDEX,   /* a[i] */
    ND_MEMBER,  /* s.m  */
    ND_ARROW,   /* p->m */
    ND_INT_LIT,
    ND_FLOAT_LIT,
    ND_STR_LIT,
    ND_IDENT,
    ND_INIT_LIST,
} NK;

/* ============================================================
 * AST node
 * ============================================================ */

typedef struct Node Node;
struct Node {
    NK     kind;
    SrcLoc loc;
    Type  *type;    /* resolved type; filled in by sema */

    /* Binary / assignment */
    Node  *left, *right;
    TK     op;      /* for ND_ASSIGN_OP: the compound operator */

    /* Control flow / loops */
    Node  *cond;
    Node  *then, *els;
    Node  *init, *update, *body;

    /* Block: array of statements */
    Node **stmts;
    int    n_stmts;

    /* Declaration / identifier */
    char   *name;
    Symbol *sym;
    int     offset;   /* stack offset (set by sema/codegen) */
    bool    is_static, is_inline, is_extern;

    /* Function definition */
    Node **params;
    int    n_params;

    /* Variable initializer */
    Node  *init_expr;

    /* Function call */
    Node  *callee;
    Node **args;
    int    n_args;

    /* Literals */
    long long  ival;
    long double fval;
    char       *sval;
    int         slen;
    int         str_id;   /* index into global string table */

    /* sizeof / _Alignof applied to a type (not an expression) */
    Type  *sizeof_type;

    /* switch/case */
    long long case_val;

    /* Member access */
    Member *member;
};

/* ============================================================
 * Global string literal table
 * ============================================================ */

typedef struct { char *data; int len; int id; } StrEntry;
extern StrEntry *str_table;
extern int       str_count;
int str_intern(const char *data, int len);

/* ============================================================
 * Compiler pipeline functions
 * ============================================================ */

char  *preprocess(const char *filename, const char *src);
Token *lex       (const char *filename, const char *src);
Node  *parse     (Token *toks);
void   sema      (Node *prog);
void   codegen   (Node *prog, FILE *out);

/* ============================================================
 * Utilities  (implemented in neutron.c)
 * ============================================================ */

_Noreturn void die    (const char *fmt, ...);
_Noreturn void error_at(SrcLoc loc, const char *fmt, ...);
void           warn_at (SrcLoc loc, const char *fmt, ...);

void *xmalloc (size_t n);
void *xcalloc (size_t count, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup (const char *s);
char *xstrndup(const char *s, size_t n);

/* Node constructors */
Node *new_node      (NK kind, SrcLoc loc);
Node *new_binary    (NK kind, Node *l, Node *r, SrcLoc loc);
Node *new_unary     (NK kind, Node *operand, SrcLoc loc);
Node *new_int_lit   (long long val, SrcLoc loc);
Node *new_float_lit (long double val, SrcLoc loc);

/* Driver entry points */
int compiler_main(int argc, char **argv);
int linker_main  (int argc, char **argv);
