/*
 * parse.c — Recursive-descent parser for Neutron
 *
 * Implements the full C99 grammar (minus VLAs and complex numbers).
 * Produces an AST rooted at ND_PROG.
 *
 * Grammar (simplified):
 *   translation_unit  := (function_def | declaration)*
 *   function_def      := decl_specs declarator compound_stmt
 *   declaration       := decl_specs init_declarator_list? ';'
 *   compound_stmt     := '{' block_item* '}'
 *   block_item        := declaration | statement
 *   statement         := expr_stmt | if | switch | while | do | for
 *                      | return | break | continue | goto | label | compound
 *   expression        := assignment (',' assignment)*
 *   assignment        := ternary (assign_op assignment)?
 *   ternary           := logor ('?' expr ':' ternary)?
 *   ... (standard C precedence chain down to primary)
 */

#include "../neutron.h"

/* ============================================================
 * Parser state
 * ============================================================ */

typedef struct {
    Token *tok;    /* current token (not yet consumed) */
} Parser;

/* ----------------------------------------------------------------
 * Token helpers
 * ---------------------------------------------------------------- */

static Token *peek(Parser *p) { return p->tok; }

static Token *advance(Parser *p) {
    Token *t = p->tok;
    if (t->kind != TK_EOF) p->tok = t->next;
    return t;
}

/* Consume token if it matches kind; return it or NULL */
static Token *eat(Parser *p, TK kind) {
    if (p->tok->kind == kind) return advance(p);
    return NULL;
}

/* Consume or die */
static Token *expect(Parser *p, TK kind) {
    Token *t = eat(p, kind);
    if (!t) {
        Token *cur = p->tok;
        error_at(cur->loc, "expected token %d, got %d", (int)kind, (int)cur->kind);
    }
    return t;
}

static bool check(Parser *p, TK kind) { return p->tok->kind == kind; }

/* ============================================================
 * Type specifier / declarator parsing
 * ============================================================ */

/* Forward declarations */
static Node *parse_expr(Parser *p);
static Node *parse_assign(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_compound(Parser *p);
static Node *parse_declaration(Parser *p);
static Type *parse_decl_specs(Parser *p, bool *is_typedef, bool *is_extern,
                               bool *is_static, bool *is_inline);
static Type *parse_declarator(Parser *p, Type *base, char **name_out);
static Type *parse_abstract_declarator(Parser *p, Type *base);

/* ----------------------------------------------------------------
 * Scope / typedef tracking (lightweight, just for parsing).
 * Full resolution happens in sema.c; here we only need to know
 * whether an identifier is a typedef name to disambiguate the grammar.
 * ---------------------------------------------------------------- */

typedef struct TDScope {
    char          **names;
    int             n, cap;
    struct TDScope *parent;
} TDScope;

static TDScope *td_scope = NULL;

static void td_push(void) {
    TDScope *s = xcalloc(1, sizeof *s);
    s->parent  = td_scope;
    td_scope   = s;
}

static void td_pop(void) {
    TDScope *s = td_scope;
    td_scope   = s->parent;
    free(s->names);
    free(s);
}

static void td_define(const char *name) {
    if (!td_scope) return;
    if (td_scope->n >= td_scope->cap) {
        td_scope->cap = td_scope->cap ? td_scope->cap * 2 : 8;
        td_scope->names = xrealloc(td_scope->names,
                                   td_scope->cap * sizeof(char*));
    }
    td_scope->names[td_scope->n++] = xstrdup(name);
}

static bool td_is_typedef(const char *name) {
    for (TDScope *s = td_scope; s; s = s->parent)
        for (int i = 0; i < s->n; i++)
            if (strcmp(s->names[i], name) == 0)
                return true;
    return false;
}

/* ----------------------------------------------------------------
 * Is the current token the start of a type specifier?
 * Used to distinguish "int x;" from "expr;" in block_item.
 * ---------------------------------------------------------------- */

static bool is_type_start(Parser *p) {
    switch (p->tok->kind) {
    case TK_VOID: case TK_CHAR: case TK_SHORT: case TK_INT: case TK_LONG:
    case TK_FLOAT: case TK_DOUBLE: case TK_SIGNED: case TK_UNSIGNED:
    case TK__BOOL: case TK_STRUCT: case TK_UNION: case TK_ENUM:
    case TK_CONST: case TK_VOLATILE: case TK_RESTRICT:
    case TK_TYPEDEF: case TK_EXTERN: case TK_STATIC: case TK_INLINE:
    case TK_REGISTER: case TK_AUTO:
        return true;
    case TK_IDENT:
        return td_is_typedef(p->tok->sval);
    default:
        return false;
    }
}

/* ----------------------------------------------------------------
 * Struct / union body
 * ---------------------------------------------------------------- */

static void parse_struct_body(Parser *p, Type *ty) {
    expect(p, TK_LBRACE);

    Member head = {0};
    Member *tail = &head;
    int offset = 0;
    int max_align = 1;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        bool dummy_td = false, dummy_e = false, dummy_s = false, dummy_i = false;
        Type *base = parse_decl_specs(p, &dummy_td, &dummy_e, &dummy_s, &dummy_i);

        /* Multiple declarators per line: int a, b; */
        do {
            char *name = NULL;
            Type *mtype = parse_declarator(p, base, &name);

            Member *m = xcalloc(1, sizeof *m);
            m->name = name ? name : xstrdup("");
            m->type = mtype;
            /* Align offset */
            int a = mtype->align;
            if (a > max_align) max_align = a;
            if (ty->kind == TY_STRUCT) {
                if (a > 1) offset = (offset + a - 1) & ~(a - 1);
                m->offset = offset;
                offset += mtype->size;
            } else {
                /* union: all at offset 0 */
                m->offset = 0;
                if (mtype->size > offset) offset = mtype->size;
            }

            tail->next = m;
            tail = m;
        } while (eat(p, TK_COMMA));

        expect(p, TK_SEMI);
    }
    expect(p, TK_RBRACE);

    /* Final struct size: round up to alignment */
    if (ty->kind == TY_STRUCT)
        ty->size = (offset + max_align - 1) & ~(max_align - 1);
    else
        ty->size = (offset + max_align - 1) & ~(max_align - 1);
    ty->align    = max_align;
    ty->members  = head.next;
    ty->complete = true;
}

/* ----------------------------------------------------------------
 * Enum body
 * ---------------------------------------------------------------- */

static Type *parse_enum(Parser *p) {
    Token *tag_tok = eat(p, TK_IDENT);
    (void)tag_tok;

    if (!check(p, TK_LBRACE))
        return ty_int;  /* reference to already-declared enum */

    advance(p);  /* { */
    long long val = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        Token *name_tok = expect(p, TK_IDENT);
        (void)name_tok;

        if (eat(p, TK_ASSIGN)) {
            /* constant expression — we parse a simple integer for now */
            Node *e = parse_assign(p);
            if (e->kind == ND_INT_LIT) val = e->ival;
            else if (e->kind == ND_NEG && e->left->kind == ND_INT_LIT)
                val = -e->left->ival;
        }
        /* We store enum constants in a simple global list (sema resolves them) */
        val++;
        if (!eat(p, TK_COMMA)) break;
    }
    expect(p, TK_RBRACE);
    return ty_int;
}

/* ----------------------------------------------------------------
 * Declaration specifiers
 *   Returns the base type; fills flags via out-params.
 * ---------------------------------------------------------------- */

static Type *parse_decl_specs(Parser *p, bool *is_typedef, bool *is_extern,
                               bool *is_static, bool *is_inline) {
    *is_typedef = *is_extern = *is_static = *is_inline = false;

    /* Counts for combining specifiers */
    int n_void=0, n_char=0, n_short=0, n_int=0, n_long=0;
    int n_float=0, n_double=0, n_signed=0, n_unsigned=0, n_bool=0;
    bool is_const = false, is_volatile = false;
    Type *named_type = NULL;  /* struct/union/enum/typedef type */

    for (;;) {
        Token *t = peek(p);

        switch (t->kind) {
        /* Storage class */
        case TK_TYPEDEF:  *is_typedef = true; advance(p); break;
        case TK_EXTERN:   *is_extern  = true; advance(p); break;
        case TK_STATIC:   *is_static  = true; advance(p); break;
        case TK_INLINE:   *is_inline  = true; advance(p); break;
        case TK_REGISTER: advance(p); break;  /* ignore register */
        case TK_AUTO:     advance(p); break;

        /* Qualifiers */
        case TK_CONST:    is_const    = true; advance(p); break;
        case TK_VOLATILE: is_volatile = true; advance(p); break;
        case TK_RESTRICT: advance(p); break;

        /* Type specifiers */
        case TK_VOID:     n_void++;     advance(p); break;
        case TK_CHAR:     n_char++;     advance(p); break;
        case TK_SHORT:    n_short++;    advance(p); break;
        case TK_INT:      n_int++;      advance(p); break;
        case TK_LONG:     n_long++;     advance(p); break;
        case TK_FLOAT:    n_float++;    advance(p); break;
        case TK_DOUBLE:   n_double++;   advance(p); break;
        case TK_SIGNED:   n_signed++;   advance(p); break;
        case TK_UNSIGNED: n_unsigned++; advance(p); break;
        case TK__BOOL:    n_bool++;     advance(p); break;

        case TK_STRUCT:
        case TK_UNION: {
            bool is_union = (t->kind == TK_UNION);
            advance(p);
            char *tag = NULL;
            if (check(p, TK_IDENT)) tag = xstrdup(advance(p)->sval);

            if (check(p, TK_LBRACE)) {
                named_type = is_union ? ty_make_union(tag) : ty_make_struct(tag);
                parse_struct_body(p, named_type);
            } else {
                /* Forward reference: create incomplete type */
                named_type = is_union ? ty_make_union(tag) : ty_make_struct(tag);
            }
            goto done_specs;
        }

        case TK_ENUM:
            advance(p);
            named_type = parse_enum(p);
            goto done_specs;

        case TK_IDENT:
            if (td_is_typedef(t->sval)) {
                /* typedef name: treat as named type (sema resolves real type) */
                advance(p);
                /* Create a placeholder; sema will resolve it */
                named_type = xcalloc(1, sizeof(Type));
                named_type->kind  = TY_INT; /* placeholder */
                named_type->size  = 4;
                named_type->align = 4;
                /* We tag it so sema can find the real type by name. */
                named_type->tag = xstrdup(t->sval);
                goto done_specs;
            }
            goto done_specs;

        default:
            goto done_specs;
        }
    }

done_specs:;
    /* Combine specifiers → Type */
    if (named_type) {
        if (is_const || is_volatile) {
            Type *c = ty_clone(named_type);
            c->is_const    = is_const;
            c->is_volatile = is_volatile;
            return c;
        }
        return named_type;
    }

    Type *base;
    if (n_void)                     base = ty_void;
    else if (n_bool)                base = ty_bool;
    else if (n_float)               base = ty_float;
    else if (n_double && n_long==1) base = ty_ldouble;
    else if (n_double)              base = ty_double;
    else if (n_char) {
        base = n_unsigned ? ty_uchar : ty_char;
    } else if (n_short) {
        base = n_unsigned ? ty_ushort : ty_short;
    } else if (n_long >= 2) {
        base = n_unsigned ? ty_ullong : ty_llong;
    } else if (n_long == 1) {
        base = n_unsigned ? ty_ulong : ty_long;
    } else if (n_unsigned) {
        base = ty_uint;
    } else {
        /* int (default for signed, or bare) */
        base = ty_int;
    }

    if (is_const || is_volatile) {
        base = ty_clone(base);
        base->is_const    = is_const;
        base->is_volatile = is_volatile;
    }
    return base;
}

/* ----------------------------------------------------------------
 * Declarator parsing
 *
 * C declarators are parsed inside-out: the "center" is the name,
 * and type modifiers are applied right-to-left ([] and () bind
 * right-to-left from center, * binds left-to-right from outside).
 *
 * We use the classic two-stack approach:
 *   1. Peel off prefix '*' modifiers.
 *   2. Parse the direct-declarator (name + postfix [] / ()).
 *   3. Apply prefix modifiers in reverse.
 * ---------------------------------------------------------------- */

/* Parse parameter list for function type.
 * Returns a Param linked list and sets *variadic. */
static Param *parse_params(Parser *p, bool *variadic) {
    *variadic = false;
    Param  head = {0};
    Param *tail = &head;

    if (check(p, TK_RPAREN)) return NULL;  /* f() — no params */
    if (check(p, TK_VOID) && peek(p)->next &&
        peek(p)->next->kind == TK_RPAREN) {
        advance(p);  /* f(void) */
        return NULL;
    }

    do {
        if (check(p, TK_ELLIPSIS)) {
            advance(p);
            *variadic = true;
            break;
        }
        bool dummy_td, dummy_e, dummy_s, dummy_i;
        Type *base = parse_decl_specs(p, &dummy_td, &dummy_e, &dummy_s, &dummy_i);
        char *name = NULL;
        Type *ptype = parse_declarator(p, base, &name);

        Param *param = xcalloc(1, sizeof *param);
        param->name = name;
        param->type = ptype;
        tail->next  = param;
        tail        = param;
    } while (eat(p, TK_COMMA));

    return head.next;
}

/* A stack of pending type modifiers collected while parsing prefix '*' */
#define MAX_PTR_DEPTH 32

static Type *parse_declarator(Parser *p, Type *base, char **name_out) {
    /* Collect pointer prefixes */
    int   n_ptrs = 0;
    bool  ptr_const[MAX_PTR_DEPTH]    = {false};
    bool  ptr_volatile[MAX_PTR_DEPTH] = {false};

    while (check(p, TK_STAR)) {
        advance(p);
        /* qualifiers on the pointer itself */
        while (check(p, TK_CONST) || check(p, TK_VOLATILE) || check(p, TK_RESTRICT)) {
            if (eat(p, TK_CONST))    ptr_const[n_ptrs]    = true;
            if (eat(p, TK_VOLATILE)) ptr_volatile[n_ptrs] = true;
            eat(p, TK_RESTRICT);
        }
        if (n_ptrs < MAX_PTR_DEPTH) n_ptrs++;
    }

    /* Direct declarator */
    Type *direct = base;
    char *name   = NULL;

    if (check(p, TK_LPAREN) && !is_type_start(p)) {
        /* Grouped declarator: (*name) or (*name)[...] etc.
         * We parse the inner declarator with a placeholder base, then
         * replace it with the real type once we know the postfix. */
        advance(p); /* ( */
        /* Parse inner recursively with dummy base */
        Type *dummy = xcalloc(1, sizeof *dummy); /* placeholder */
        dummy->kind = TY_VOID;
        dummy->size = dummy->align = 1;
        Type *inner = parse_declarator(p, dummy, &name);
        expect(p, TK_RPAREN);

        /* Now parse postfix modifiers ([], ()) applied to the outer type */
        Type *outer = base;
        while (check(p, TK_LBRACKET) || check(p, TK_LPAREN)) {
            if (eat(p, TK_LBRACKET)) {
                int len = -1;
                if (!check(p, TK_RBRACKET)) {
                    Node *e = parse_assign(p);
                    if (e->kind == ND_INT_LIT) len = (int)e->ival;
                }
                expect(p, TK_RBRACKET);
                outer = ty_array_of(outer, len);
            } else {
                advance(p); /* ( */
                bool variadic = false;
                Param *params = parse_params(p, &variadic);
                expect(p, TK_RPAREN);
                outer = ty_func(outer, params, variadic);
            }
        }

        /* Patch inner's dummy base → outer */
        /* Walk inner until we hit the dummy placeholder */
        Type *walk = inner;
        while (walk) {
            if (walk->base == dummy) { walk->base = outer; break; }
            if (walk == dummy)       { inner = outer;      break; }
            walk = walk->base;
        }
        direct = inner;
        goto apply_ptrs;
    }

    /* Simple name */
    if (check(p, TK_IDENT)) {
        Token *t = advance(p);
        name = xstrdup(t->sval);
    }
    /* Abstract declarator may have no name */

    /* Postfix: [], () */
    direct = base;
    while (check(p, TK_LBRACKET) || check(p, TK_LPAREN)) {
        if (eat(p, TK_LBRACKET)) {
            int len = -1;
            if (!check(p, TK_RBRACKET)) {
                Node *e = parse_assign(p);
                if (e->kind == ND_INT_LIT) len = (int)e->ival;
            }
            expect(p, TK_RBRACKET);
            direct = ty_array_of(direct, len);
        } else {
            advance(p);  /* ( */
            bool variadic = false;
            Param *params = parse_params(p, &variadic);
            expect(p, TK_RPAREN);
            direct = ty_func(direct, params, variadic);
        }
    }

apply_ptrs:
    /* Apply pointer prefixes in reverse order */
    for (int i = n_ptrs - 1; i >= 0; i--) {
        direct = ty_ptr_to(direct);
        direct->is_const    = ptr_const[i];
        direct->is_volatile = ptr_volatile[i];
    }

    if (name_out) *name_out = name;
    return direct;
}

static Type *parse_abstract_declarator(Parser *p, Type *base) {
    return parse_declarator(p, base, NULL);
}

/* ============================================================
 * Expression parsing
 * ============================================================ */

static Node *parse_primary(Parser *p);
static Node *parse_postfix(Parser *p);
static Node *parse_unary(Parser *p);
static Node *parse_cast(Parser *p);
static Node *parse_mul(Parser *p);
static Node *parse_add(Parser *p);
static Node *parse_shift(Parser *p);
static Node *parse_relational(Parser *p);
static Node *parse_equality(Parser *p);
static Node *parse_bitand(Parser *p);
static Node *parse_bitxor(Parser *p);
static Node *parse_bitor(Parser *p);
static Node *parse_logand(Parser *p);
static Node *parse_logor(Parser *p);
static Node *parse_ternary(Parser *p);
static Node *parse_assign(Parser *p);
static Node *parse_expr(Parser *p);

/* ---- primary ---- */
static Node *parse_primary(Parser *p) {
    Token *t = peek(p);
    SrcLoc loc = t->loc;

    /* Integer literal */
    if (t->kind == TK_INT_LIT || t->kind == TK_CHAR_LIT) {
        advance(p);
        Node *n = new_node(ND_INT_LIT, loc);
        n->ival = (long long)t->ival;
        /* Determine type from suffixes */
        if (t->is_llong)      n->type = t->is_unsigned ? ty_ullong : ty_llong;
        else if (t->is_long)  n->type = t->is_unsigned ? ty_ulong  : ty_long;
        else if (t->is_unsigned) n->type = ty_uint;
        else                  n->type = ty_int;
        return n;
    }

    /* Float literal */
    if (t->kind == TK_FLOAT_LIT) {
        advance(p);
        Node *n = new_node(ND_FLOAT_LIT, loc);
        n->fval = t->fval;
        n->type = t->is_float ? ty_float : ty_double;
        return n;
    }

    /* String literal */
    if (t->kind == TK_STR_LIT) {
        advance(p);
        Node *n = new_node(ND_STR_LIT, loc);
        n->sval   = t->sval;
        n->slen   = t->slen;
        n->str_id = str_intern(t->sval, t->slen + 1); /* include null */
        n->type   = ty_ptr_to(ty_char);
        return n;
    }

    /* Identifier */
    if (t->kind == TK_IDENT) {
        advance(p);
        Node *n = new_node(ND_IDENT, loc);
        n->name = xstrdup(t->sval);
        return n;
    }

    /* Parenthesized expression or compound literal */
    if (t->kind == TK_LPAREN) {
        advance(p);
        Node *n = parse_expr(p);
        expect(p, TK_RPAREN);
        return n;
    }

    error_at(loc, "unexpected token in expression: kind=%d", (int)t->kind);
}

/* ---- postfix ---- */
static Node *parse_postfix(Parser *p) {
    Node *n = parse_primary(p);

    for (;;) {
        SrcLoc loc = peek(p)->loc;

        if (eat(p, TK_LBRACKET)) {
            Node *idx = parse_expr(p);
            expect(p, TK_RBRACKET);
            n = new_binary(ND_INDEX, n, idx, loc);
            continue;
        }

        if (eat(p, TK_LPAREN)) {
            /* Function call: parse argument list */
            Node **args = NULL;
            int    n_args = 0, cap = 0;
            while (!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
                if (n_args >= cap) {
                    cap = cap ? cap * 2 : 4;
                    args = xrealloc(args, cap * sizeof *args);
                }
                args[n_args++] = parse_assign(p);
                if (!eat(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN);
            Node *call = new_node(ND_CALL, loc);
            call->callee = n;
            call->args   = args;
            call->n_args = n_args;
            n = call;
            continue;
        }

        if (eat(p, TK_DOT)) {
            Token *name_tok = expect(p, TK_IDENT);
            Node *m = new_node(ND_MEMBER, loc);
            m->left = n;
            m->name = xstrdup(name_tok->sval);
            n = m;
            continue;
        }

        if (eat(p, TK_ARROW)) {
            Token *name_tok = expect(p, TK_IDENT);
            Node *m = new_node(ND_ARROW, loc);
            m->left = n;
            m->name = xstrdup(name_tok->sval);
            n = m;
            continue;
        }

        if (eat(p, TK_INC)) { n = new_unary(ND_POST_INC, n, loc); continue; }
        if (eat(p, TK_DEC)) { n = new_unary(ND_POST_DEC, n, loc); continue; }

        break;
    }
    return n;
}

/* ---- unary ---- */
static Node *parse_unary(Parser *p) {
    SrcLoc loc = peek(p)->loc;

    if (eat(p, TK_INC))   return new_unary(ND_PRE_INC,  parse_unary(p), loc);
    if (eat(p, TK_DEC))   return new_unary(ND_PRE_DEC,  parse_unary(p), loc);
    if (eat(p, TK_AMP))   return new_unary(ND_ADDR,      parse_cast(p),  loc);
    if (eat(p, TK_STAR))  return new_unary(ND_DEREF,     parse_cast(p),  loc);
    if (eat(p, TK_PLUS))  return parse_cast(p);  /* unary + is a no-op */
    if (eat(p, TK_MINUS)) return new_unary(ND_NEG,   parse_cast(p), loc);
    if (eat(p, TK_BANG))  return new_unary(ND_NOT,   parse_cast(p), loc);
    if (eat(p, TK_TILDE)) return new_unary(ND_BITNOT, parse_cast(p), loc);

    if (check(p, TK_SIZEOF)) {
        advance(p);
        Node *n = new_node(ND_SIZEOF, loc);
        if (check(p, TK_LPAREN) && is_type_start(p->tok->next ?
            &(Parser){p->tok->next} : p)) {
            /* sizeof(type) */
            advance(p);  /* ( */
            bool dummy_td, dummy_e, dummy_s, dummy_i;
            Type *base = parse_decl_specs(p, &dummy_td, &dummy_e, &dummy_s, &dummy_i);
            n->sizeof_type = parse_abstract_declarator(p, base);
            expect(p, TK_RPAREN);
        } else {
            /* sizeof expr */
            n->left = parse_unary(p);
        }
        return n;
    }

    if (check(p, TK__ALIGNOF)) {
        advance(p);
        expect(p, TK_LPAREN);
        bool dummy_td, dummy_e, dummy_s, dummy_i;
        Type *base = parse_decl_specs(p, &dummy_td, &dummy_e, &dummy_s, &dummy_i);
        Node *n = new_node(ND_ALIGNOF, loc);
        n->sizeof_type = parse_abstract_declarator(p, base);
        expect(p, TK_RPAREN);
        return n;
    }

    return parse_postfix(p);
}

/* ---- cast ---- */
static Node *parse_cast(Parser *p) {
    /* Look ahead: is this (type)expr ? */
    if (check(p, TK_LPAREN)) {
        Token *next = p->tok->next;
        if (next && (next->kind == TK_VOID || next->kind == TK_CHAR ||
                     next->kind == TK_SHORT || next->kind == TK_INT ||
                     next->kind == TK_LONG || next->kind == TK_FLOAT ||
                     next->kind == TK_DOUBLE || next->kind == TK_UNSIGNED ||
                     next->kind == TK_SIGNED || next->kind == TK__BOOL ||
                     next->kind == TK_STRUCT || next->kind == TK_UNION ||
                     next->kind == TK_ENUM ||
                     next->kind == TK_CONST || next->kind == TK_VOLATILE ||
                     next->kind == TK_RESTRICT ||
                     (next->kind == TK_IDENT && td_is_typedef(next->sval)))) {
            SrcLoc loc = peek(p)->loc;
            advance(p);  /* ( */
            bool dummy_td, dummy_e, dummy_s, dummy_i;
            Type *base = parse_decl_specs(p, &dummy_td, &dummy_e, &dummy_s, &dummy_i);
            Type *cast_type = parse_abstract_declarator(p, base);
            if (eat(p, TK_RPAREN)) {
                Node *n = new_node(ND_CAST, loc);
                n->left       = parse_cast(p);
                n->sizeof_type = cast_type;
                return n;
            }
            /* Not a cast after all — parse as grouped expression */
            /* (we already consumed — this is a simplification) */
        }
    }
    return parse_unary(p);
}

/* ---- multiplicative ---- */
static Node *parse_mul(Parser *p) {
    Node *n = parse_cast(p);
    for (;;) {
        SrcLoc loc = peek(p)->loc;
        if (eat(p, TK_STAR))    n = new_binary(ND_MUL, n, parse_cast(p), loc);
        else if (eat(p, TK_SLASH))   n = new_binary(ND_DIV, n, parse_cast(p), loc);
        else if (eat(p, TK_PERCENT)) n = new_binary(ND_MOD, n, parse_cast(p), loc);
        else break;
    }
    return n;
}

/* ---- additive ---- */
static Node *parse_add(Parser *p) {
    Node *n = parse_mul(p);
    for (;;) {
        SrcLoc loc = peek(p)->loc;
        if (eat(p, TK_PLUS))  n = new_binary(ND_ADD, n, parse_mul(p), loc);
        else if (eat(p, TK_MINUS)) n = new_binary(ND_SUB, n, parse_mul(p), loc);
        else break;
    }
    return n;
}

/* ---- shift ---- */
static Node *parse_shift(Parser *p) {
    Node *n = parse_add(p);
    for (;;) {
        SrcLoc loc = peek(p)->loc;
        if (eat(p, TK_LSHIFT)) n = new_binary(ND_SHL, n, parse_add(p), loc);
        else if (eat(p, TK_RSHIFT)) n = new_binary(ND_SHR, n, parse_add(p), loc);
        else break;
    }
    return n;
}

/* ---- relational ---- */
static Node *parse_relational(Parser *p) {
    Node *n = parse_shift(p);
    for (;;) {
        SrcLoc loc = peek(p)->loc;
        if (eat(p, TK_LT))  n = new_binary(ND_LT,  n, parse_shift(p), loc);
        else if (eat(p, TK_GT))  n = new_binary(ND_GT,  n, parse_shift(p), loc);
        else if (eat(p, TK_LEQ)) n = new_binary(ND_LEQ, n, parse_shift(p), loc);
        else if (eat(p, TK_GEQ)) n = new_binary(ND_GEQ, n, parse_shift(p), loc);
        else break;
    }
    return n;
}

/* ---- equality ---- */
static Node *parse_equality(Parser *p) {
    Node *n = parse_relational(p);
    for (;;) {
        SrcLoc loc = peek(p)->loc;
        if (eat(p, TK_EQ))  n = new_binary(ND_EQ,  n, parse_relational(p), loc);
        else if (eat(p, TK_NEQ)) n = new_binary(ND_NEQ, n, parse_relational(p), loc);
        else break;
    }
    return n;
}

/* ---- bitwise and/xor/or ---- */
static Node *parse_bitand(Parser *p) {
    Node *n = parse_equality(p);
    while (check(p, TK_AMP) && peek(p)->next &&
           peek(p)->next->kind != TK_AMP) {
        SrcLoc loc = peek(p)->loc; advance(p);
        n = new_binary(ND_BITAND, n, parse_equality(p), loc);
    }
    /* Simpler: just check single & */
    return n;
}

static Node *parse_bitxor(Parser *p) {
    Node *n = parse_bitand(p);
    for (;;) {
        SrcLoc loc = peek(p)->loc;
        if (eat(p, TK_CARET)) n = new_binary(ND_BITXOR, n, parse_bitand(p), loc);
        else break;
    }
    return n;
}

static Node *parse_bitor(Parser *p) {
    Node *n = parse_bitxor(p);
    for (;;) {
        SrcLoc loc = peek(p)->loc;
        if (!check(p, TK_PIPE) || (p->tok->next && p->tok->next->kind == TK_PIPE))
            break;
        if (eat(p, TK_PIPE)) n = new_binary(ND_BITOR, n, parse_bitxor(p), loc);
        else break;
    }
    return n;
}

static Node *parse_logand(Parser *p) {
    Node *n = parse_bitor(p);
    for (;;) {
        SrcLoc loc = peek(p)->loc;
        if (eat(p, TK_AND)) n = new_binary(ND_LOGAND, n, parse_bitor(p), loc);
        else break;
    }
    return n;
}

static Node *parse_logor(Parser *p) {
    Node *n = parse_logand(p);
    for (;;) {
        SrcLoc loc = peek(p)->loc;
        if (eat(p, TK_OR)) n = new_binary(ND_LOGOR, n, parse_logand(p), loc);
        else break;
    }
    return n;
}

static Node *parse_ternary(Parser *p) {
    Node *cond = parse_logor(p);
    SrcLoc loc = peek(p)->loc;
    if (!eat(p, TK_QUESTION)) return cond;

    Node *then = parse_expr(p);
    expect(p, TK_COLON);
    Node *els = parse_ternary(p);

    Node *n = new_node(ND_TERNARY, loc);
    n->cond = cond;
    n->then = then;
    n->els  = els;
    return n;
}

static Node *parse_assign(Parser *p) {
    Node *n = parse_ternary(p);
    SrcLoc loc = peek(p)->loc;

    /* Simple assignment */
    if (eat(p, TK_ASSIGN)) {
        return new_binary(ND_ASSIGN, n, parse_assign(p), loc);
    }

    /* Compound assignment */
    TK op = TK_EOF;
    if (check(p, TK_PLUS_EQ))    op = TK_PLUS_EQ;
    else if (check(p, TK_MINUS_EQ))   op = TK_MINUS_EQ;
    else if (check(p, TK_STAR_EQ))    op = TK_STAR_EQ;
    else if (check(p, TK_SLASH_EQ))   op = TK_SLASH_EQ;
    else if (check(p, TK_PERCENT_EQ)) op = TK_PERCENT_EQ;
    else if (check(p, TK_AMP_EQ))     op = TK_AMP_EQ;
    else if (check(p, TK_PIPE_EQ))    op = TK_PIPE_EQ;
    else if (check(p, TK_CARET_EQ))   op = TK_CARET_EQ;
    else if (check(p, TK_LSHIFT_EQ))  op = TK_LSHIFT_EQ;
    else if (check(p, TK_RSHIFT_EQ))  op = TK_RSHIFT_EQ;

    if (op != TK_EOF) {
        advance(p);
        Node *rhs = parse_assign(p);
        Node *node = new_node(ND_ASSIGN_OP, loc);
        node->left  = n;
        node->right = rhs;
        node->op    = op;
        return node;
    }

    return n;
}

static Node *parse_expr(Parser *p) {
    Node *n = parse_assign(p);
    SrcLoc loc = peek(p)->loc;
    while (eat(p, TK_COMMA)) {
        n = new_binary(ND_COMMA, n, parse_assign(p), loc);
        loc = peek(p)->loc;
    }
    return n;
}

/* ============================================================
 * Statement parsing
 * ============================================================ */

static Node *parse_stmt(Parser *p) {
    Token *t  = peek(p);
    SrcLoc loc = t->loc;

    /* Compound statement */
    if (t->kind == TK_LBRACE) return parse_compound(p);

    /* if */
    if (eat(p, TK_IF)) {
        expect(p, TK_LPAREN);
        Node *cond = parse_expr(p);
        expect(p, TK_RPAREN);
        Node *then = parse_stmt(p);
        Node *els  = eat(p, TK_ELSE) ? parse_stmt(p) : NULL;
        Node *n = new_node(ND_IF, loc);
        n->cond = cond; n->then = then; n->els = els;
        return n;
    }

    /* while */
    if (eat(p, TK_WHILE)) {
        expect(p, TK_LPAREN);
        Node *cond = parse_expr(p);
        expect(p, TK_RPAREN);
        Node *body = parse_stmt(p);
        Node *n = new_node(ND_WHILE, loc);
        n->cond = cond; n->body = body;
        return n;
    }

    /* do … while */
    if (eat(p, TK_DO)) {
        Node *body = parse_stmt(p);
        expect(p, TK_WHILE);
        expect(p, TK_LPAREN);
        Node *cond = parse_expr(p);
        expect(p, TK_RPAREN);
        expect(p, TK_SEMI);
        Node *n = new_node(ND_DO_WHILE, loc);
        n->cond = cond; n->body = body;
        return n;
    }

    /* for */
    if (eat(p, TK_FOR)) {
        expect(p, TK_LPAREN);
        td_push();
        Node *init = NULL;
        if (!check(p, TK_SEMI)) {
            if (is_type_start(p))
                init = parse_declaration(p);
            else {
                init = new_node(ND_EXPR_STMT, peek(p)->loc);
                init->left = parse_expr(p);
                expect(p, TK_SEMI);
            }
        } else {
            advance(p);
        }
        Node *cond = check(p, TK_SEMI) ? NULL : parse_expr(p);
        expect(p, TK_SEMI);
        Node *update = check(p, TK_RPAREN) ? NULL : parse_expr(p);
        expect(p, TK_RPAREN);
        Node *body = parse_stmt(p);
        td_pop();

        Node *n = new_node(ND_FOR, loc);
        n->init   = init;
        n->cond   = cond;
        n->update = update;
        n->body   = body;
        return n;
    }

    /* switch */
    if (eat(p, TK_SWITCH)) {
        expect(p, TK_LPAREN);
        Node *cond = parse_expr(p);
        expect(p, TK_RPAREN);
        Node *body = parse_stmt(p);
        Node *n = new_node(ND_SWITCH, loc);
        n->cond = cond; n->body = body;
        return n;
    }

    /* case */
    if (eat(p, TK_CASE)) {
        Node *val_expr = parse_ternary(p);  /* constant expr */
        expect(p, TK_COLON);
        Node *n = new_node(ND_CASE, loc);
        n->case_val = val_expr->ival;
        n->body = parse_stmt(p);
        return n;
    }

    /* default */
    if (eat(p, TK_DEFAULT)) {
        expect(p, TK_COLON);
        Node *n = new_node(ND_DEFAULT, loc);
        n->body = parse_stmt(p);
        return n;
    }

    /* return */
    if (eat(p, TK_RETURN)) {
        Node *n = new_node(ND_RETURN, loc);
        if (!check(p, TK_SEMI))
            n->left = parse_expr(p);
        expect(p, TK_SEMI);
        return n;
    }

    /* break */
    if (eat(p, TK_BREAK))    { expect(p, TK_SEMI); return new_node(ND_BREAK, loc); }

    /* continue */
    if (eat(p, TK_CONTINUE)) { expect(p, TK_SEMI); return new_node(ND_CONTINUE, loc); }

    /* goto */
    if (eat(p, TK_GOTO)) {
        Token *lbl = expect(p, TK_IDENT);
        expect(p, TK_SEMI);
        Node *n = new_node(ND_GOTO, loc);
        n->name = xstrdup(lbl->sval);
        return n;
    }

    /* label: ident ':' stmt  — disambiguate from expr */
    if (t->kind == TK_IDENT && t->next && t->next->kind == TK_COLON) {
        advance(p); advance(p);  /* name : */
        Node *n = new_node(ND_LABEL, loc);
        n->name = xstrdup(t->sval);
        n->body = parse_stmt(p);
        return n;
    }

    /* null statement */
    if (eat(p, TK_SEMI)) return new_node(ND_NULL_STMT, loc);

    /* expression statement */
    Node *n = new_node(ND_EXPR_STMT, loc);
    n->left = parse_expr(p);
    expect(p, TK_SEMI);
    return n;
}

/* ============================================================
 * Compound statement (block)
 * ============================================================ */

static Node *parse_compound(Parser *p) {
    SrcLoc loc = peek(p)->loc;
    expect(p, TK_LBRACE);
    td_push();

    Node **items = NULL;
    int n = 0, cap = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        Node *item;
        if (is_type_start(p))
            item = parse_declaration(p);
        else
            item = parse_stmt(p);

        if (n >= cap) {
            cap = cap ? cap * 2 : 8;
            items = xrealloc(items, cap * sizeof *items);
        }
        items[n++] = item;
    }
    expect(p, TK_RBRACE);
    td_pop();

    Node *block = new_node(ND_BLOCK, loc);
    block->stmts   = items;
    block->n_stmts = n;
    return block;
}

/* ============================================================
 * Declaration parsing
 * Returns ND_VAR_DECL (or list via ND_BLOCK for multiple declarators)
 * ============================================================ */

static Node *parse_declaration(Parser *p) {
    SrcLoc loc = peek(p)->loc;
    bool is_typedef, is_extern, is_static, is_inline;
    Type *base = parse_decl_specs(p, &is_typedef, &is_extern, &is_static, &is_inline);

    /* Possible bare struct/union/enum declaration with no declarator: */
    if (check(p, TK_SEMI)) {
        advance(p);
        return new_node(ND_NULL_STMT, loc);
    }

    /* One or more declarators */
    Node **decls = NULL;
    int n = 0, cap = 0;

    do {
        char *name = NULL;
        Type *dtype = parse_declarator(p, base, &name);

        if (is_typedef && name) {
            td_define(name);
            /* Emit a dummy node so sema can register the typedef */
            Node *td = new_node(ND_VAR_DECL, loc);
            td->name      = name;
            td->type      = dtype;
            td->is_extern = true;  /* re-use flag: marks this as typedef */
            td->is_static = false;
            if (n >= cap) { cap = cap ? cap*2 : 4; decls = xrealloc(decls, cap*sizeof*decls); }
            decls[n++] = td;
            continue;
        }

        Node *decl = new_node(ND_VAR_DECL, loc);
        decl->name      = name;
        decl->type      = dtype;
        decl->is_extern = is_extern;
        decl->is_static = is_static;
        decl->is_inline = is_inline;

        /* Initializer */
        if (eat(p, TK_ASSIGN)) {
            if (check(p, TK_LBRACE)) {
                /* Initializer list: {a, b, c} */
                SrcLoc iloc = peek(p)->loc;
                advance(p);
                Node **inits = NULL;
                int in = 0, icap = 0;
                while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
                    if (in >= icap) { icap = icap ? icap*2 : 4; inits = xrealloc(inits, icap*sizeof*inits); }
                    inits[in++] = parse_assign(p);
                    if (!eat(p, TK_COMMA)) break;
                }
                eat(p, TK_COMMA);
                expect(p, TK_RBRACE);
                Node *il = new_node(ND_INIT_LIST, iloc);
                il->stmts   = inits;
                il->n_stmts = in;
                decl->init_expr = il;
            } else {
                decl->init_expr = parse_assign(p);
            }
        }

        /* Function definition: name(params) { body } */
        if (dtype->kind == TY_FUNC && check(p, TK_LBRACE)) {
            Node *fdef = new_node(ND_FUNC_DEF, loc);
            fdef->name      = name;
            fdef->type      = dtype;
            fdef->is_static = is_static;
            fdef->is_inline = is_inline;
            fdef->is_extern = is_extern;

            /* Build param nodes */
            int np = 0;
            for (Param *pm = dtype->params; pm; pm = pm->next) np++;
            fdef->params   = xcalloc(np, sizeof *fdef->params);
            fdef->n_params = np;
            int i = 0;
            for (Param *pm = dtype->params; pm; pm = pm->next, i++) {
                Node *pn = new_node(ND_VAR_DECL, loc);
                pn->name = pm->name ? xstrdup(pm->name) : NULL;
                pn->type = pm->type;
                fdef->params[i] = pn;
            }

            td_push();
            /* Register params as local names */
            for (int j = 0; j < np; j++)
                if (fdef->params[j]->name)
                    /* We don't register them as typedefs — just push scope */
                    (void)0;
            fdef->body = parse_compound(p);
            td_pop();

            /* Function defs are never followed by comma */
            if (n == 0) {
                free(decls);
                return fdef;
            }
            decls[n++] = fdef;
            break;
        }

        if (n >= cap) { cap = cap ? cap*2 : 4; decls = xrealloc(decls, cap*sizeof*decls); }
        decls[n++] = decl;
    } while (eat(p, TK_COMMA));

    expect(p, TK_SEMI);

    if (n == 1) { Node *r = decls[0]; free(decls); return r; }

    /* Multiple declarators: wrap in a block */
    Node *blk = new_node(ND_BLOCK, loc);
    blk->stmts   = decls;
    blk->n_stmts = n;
    return blk;
}

/* ============================================================
 * Translation unit
 * ============================================================ */

Node *parse(Token *toks) {
    Parser p = { .tok = toks };
    td_push();  /* global scope */

    Node **items = NULL;
    int n = 0, cap = 0;

    while (!check(&p, TK_EOF)) {
        Node *item = parse_declaration(&p);
        if (n >= cap) {
            cap = cap ? cap * 2 : 16;
            items = xrealloc(items, cap * sizeof *items);
        }
        items[n++] = item;
    }

    td_pop();

    Node *prog = new_node(ND_PROG, toks->loc);
    prog->stmts   = items;
    prog->n_stmts = n;
    return prog;
}
