/*
 * sema.c — Semantic analysis for Neutron
 *
 * Responsibilities:
 *   1. Walk the AST top-down.
 *   2. Maintain a scope stack; resolve every ND_IDENT to a Symbol.
 *   3. Assign types to every expression node.
 *   4. Compute stack offsets for local variables.
 *   5. Insert implicit casts where the C standard requires them.
 *   6. Validate:  undeclared identifiers, incompatible types,
 *      break/continue outside loops, return type mismatches.
 */

#include "../neutron.h"

/* ============================================================
 * Scope stack
 * ============================================================ */

static Scope *cur_scope = NULL;

static void scope_push(void) {
    Scope *s = xcalloc(1, sizeof *s);
    s->parent = cur_scope;
    cur_scope = s;
}

static void scope_pop(void) {
    cur_scope = cur_scope->parent;
}

/* Declare a variable/typedef in the current scope */
static Symbol *scope_define(const char *name, Type *type, bool is_typedef) {
    Symbol *s = xcalloc(1, sizeof *s);
    s->name       = xstrdup(name);
    s->type       = type;
    s->is_typedef = is_typedef;
    s->next       = cur_scope->vars;
    cur_scope->vars = s;
    return s;
}

/* Look up a variable / typedef in all scopes */
static Symbol *scope_lookup(const char *name) {
    for (Scope *s = cur_scope; s; s = s->parent)
        for (Symbol *sym = s->vars; sym; sym = sym->next)
            if (strcmp(sym->name, name) == 0)
                return sym;
    return NULL;
}

/* Declare a struct/union/enum tag */
static Symbol *tag_define(const char *tag, Type *type) {
    Symbol *s = xcalloc(1, sizeof *s);
    s->name = xstrdup(tag);
    s->type = type;
    s->next = cur_scope->tags;
    cur_scope->tags = s;
    return s;
}

/* Look up a tag */
static Symbol *tag_lookup(const char *tag) {
    for (Scope *s = cur_scope; s; s = s->parent)
        for (Symbol *sym = s->tags; sym; sym = sym->next)
            if (strcmp(sym->name, tag) == 0)
                return sym;
    return NULL;
}

/* ============================================================
 * State: current function (for return type checking, offset tracking)
 * ============================================================ */

static Type   *cur_func_ret  = NULL;   /* return type of current function */
static int     cur_stack_off = 0;      /* current stack allocation offset  */
static int     max_stack_off = 0;      /* high water mark                  */
static int     loop_depth    = 0;      /* for break/continue validation    */
static int     switch_depth  = 0;      /* for break inside switch          */

/* Allocate space on stack for a local variable. Returns (negative) offset. */
static int alloc_local(int size, int align) {
    cur_stack_off = (cur_stack_off + size + align - 1) & ~(align - 1);
    if (cur_stack_off > max_stack_off) max_stack_off = cur_stack_off;
    return -cur_stack_off;
}

/* ============================================================
 * Implicit cast insertion
 * ============================================================ */

static Node *make_cast(Node *n, Type *to) {
    if (n->type == to) return n;
    Node *c = new_node(ND_CAST, n->loc);
    c->left        = n;
    c->type        = to;
    c->sizeof_type = to;
    return c;
}

/* Integer promotion: char/short → int */
static Node *promote(Node *n) {
    if (!n->type) return n;
    if (n->type->kind == TY_CHAR  || n->type->kind == TY_UCHAR  ||
        n->type->kind == TY_SHORT || n->type->kind == TY_USHORT ||
        n->type->kind == TY_BOOL)
        return make_cast(n, ty_int);
    return n;
}

/* ============================================================
 * Label / goto tracking
 * ============================================================ */

#define MAX_LABELS 256
static char *defined_labels[MAX_LABELS];
static char *goto_labels[MAX_LABELS];
static SrcLoc goto_locs[MAX_LABELS];
static int n_defined = 0, n_gotos = 0;

/* ============================================================
 * Forward declarations
 * ============================================================ */

static void  analyze_node(Node *n);
static void  analyze_expr(Node *n);
static Node *analyze_lvalue(Node *n);

/* ============================================================
 * Type resolution (typedef names → real types)
 * ============================================================ */

static Type *resolve_type(Type *t) {
    if (!t) return ty_int;
    /* Typedef placeholder: kind==TY_INT and tag is set */
    if ((t->kind == TY_INT || t->kind == TY_STRUCT || t->kind == TY_UNION) &&
        t->tag) {
        Symbol *sym = scope_lookup(t->tag);
        if (sym && sym->is_typedef) return sym->type;
        /* Could be a struct/union tag */
        Symbol *tag_sym = tag_lookup(t->tag);
        if (tag_sym) return tag_sym->type;
    }
    /* Recurse into pointer/array/function bases */
    if (t->base) t->base = resolve_type(t->base);
    return t;
}

/* ============================================================
 * Expression analysis — assigns t->type to every expr node
 * ============================================================ */

static void analyze_expr(Node *n) {
    if (!n) return;

    switch (n->kind) {

    case ND_INT_LIT:
    case ND_FLOAT_LIT:
    case ND_STR_LIT:
        /* Type set during parsing */
        break;

    case ND_IDENT: {
        Symbol *sym = scope_lookup(n->name);
        if (!sym)
            error_at(n->loc, "undeclared identifier '%s'", n->name);
        n->sym  = sym;
        n->type = sym->type;
        if (sym->is_typedef)
            error_at(n->loc, "'%s' is a type, not a value", n->name);
        break;
    }

    case ND_SIZEOF: {
        if (n->sizeof_type) {
            n->sizeof_type = resolve_type(n->sizeof_type);
            n->ival = n->sizeof_type->size;
        } else {
            analyze_expr(n->left);
            n->ival = n->left->type ? n->left->type->size : 0;
        }
        n->type = ty_ulong;
        n->kind = ND_INT_LIT;   /* fold to constant */
        break;
    }

    case ND_ALIGNOF: {
        n->sizeof_type = resolve_type(n->sizeof_type);
        n->ival = n->sizeof_type->align;
        n->type = ty_ulong;
        n->kind = ND_INT_LIT;
        break;
    }

    case ND_CAST: {
        analyze_expr(n->left);
        n->sizeof_type = resolve_type(n->sizeof_type);
        n->type = n->sizeof_type;
        break;
    }

    case ND_ADDR: {
        analyze_expr(n->left);
        if (!n->left->type)
            error_at(n->loc, "cannot take address of void expression");
        n->type = ty_ptr_to(n->left->type);
        break;
    }

    case ND_DEREF: {
        analyze_expr(n->left);
        Type *t = n->left->type;
        if (!t) error_at(n->loc, "dereferencing null type");
        if (t->kind == TY_PTR)   n->type = t->base;
        else if (t->kind == TY_ARRAY) n->type = t->base;
        else error_at(n->loc, "dereferencing non-pointer type");
        break;
    }

    case ND_NEG: {
        analyze_expr(n->left);
        n->left = promote(n->left);
        n->type = n->left->type;
        break;
    }

    case ND_NOT: {
        analyze_expr(n->left);
        n->type = ty_int;
        break;
    }

    case ND_BITNOT: {
        analyze_expr(n->left);
        n->left = promote(n->left);
        n->type = n->left->type;
        break;
    }

    case ND_PRE_INC: case ND_PRE_DEC:
    case ND_POST_INC: case ND_POST_DEC: {
        analyze_expr(n->left);
        n->type = n->left->type;
        break;
    }

    /* Binary arithmetic / bitwise */
    case ND_ADD: case ND_SUB:
    case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_SHL: case ND_SHR:
    case ND_BITAND: case ND_BITXOR: case ND_BITOR: {
        analyze_expr(n->left);
        analyze_expr(n->right);

        Type *lt = n->left->type, *rt = n->right->type;
        if (!lt || !rt) { n->type = ty_int; break; }

        /* Pointer arithmetic */
        if (n->kind == ND_ADD) {
            if (ty_is_pointer(lt) && ty_is_integer(rt)) {
                n->type = lt; break;
            }
            if (ty_is_integer(lt) && ty_is_pointer(rt)) {
                n->type = rt; break;
            }
        }
        if (n->kind == ND_SUB) {
            if (ty_is_pointer(lt) && ty_is_pointer(rt)) {
                n->type = ty_long; break;   /* ptrdiff_t */
            }
            if (ty_is_pointer(lt) && ty_is_integer(rt)) {
                n->type = lt; break;
            }
        }

        /* Usual arithmetic conversions */
        n->left  = promote(n->left);
        n->right = promote(n->right);
        lt = n->left->type; rt = n->right->type;
        Type *common = ty_usual_arith(lt, rt);
        if (lt != common)  n->left  = make_cast(n->left,  common);
        if (rt != common)  n->right = make_cast(n->right, common);
        n->type = common;
        break;
    }

    /* Comparisons */
    case ND_EQ: case ND_NEQ:
    case ND_LT: case ND_GT: case ND_LEQ: case ND_GEQ: {
        analyze_expr(n->left);
        analyze_expr(n->right);
        n->left  = promote(n->left);
        n->right = promote(n->right);
        n->type  = ty_int;
        break;
    }

    /* Logical */
    case ND_LOGAND: case ND_LOGOR: {
        analyze_expr(n->left);
        analyze_expr(n->right);
        n->type = ty_int;
        break;
    }

    /* Comma */
    case ND_COMMA: {
        analyze_expr(n->left);
        analyze_expr(n->right);
        n->type = n->right->type;
        break;
    }

    /* Assignment */
    case ND_ASSIGN: {
        analyze_expr(n->left);
        analyze_expr(n->right);
        if (!n->left->type) { n->type = ty_int; break; }
        /* Cast RHS to LHS type */
        if (n->right->type && n->right->type != n->left->type)
            n->right = make_cast(n->right, n->left->type);
        n->type = n->left->type;
        break;
    }

    case ND_ASSIGN_OP: {
        analyze_expr(n->left);
        analyze_expr(n->right);
        n->type = n->left->type ? n->left->type : ty_int;
        break;
    }

    /* Ternary */
    case ND_TERNARY: {
        analyze_expr(n->cond);
        analyze_expr(n->then);
        analyze_expr(n->els);
        if (n->then->type && n->els->type)
            n->type = ty_usual_arith(n->then->type, n->els->type);
        else
            n->type = n->then->type ? n->then->type : ty_int;
        break;
    }

    /* Array subscript a[i] → *(a + i) */
    case ND_INDEX: {
        analyze_expr(n->left);
        analyze_expr(n->right);
        Type *t = n->left->type;
        if (t && t->kind == TY_ARRAY)  n->type = t->base;
        else if (t && t->kind == TY_PTR) n->type = t->base;
        else { warn_at(n->loc, "subscript of non-array/pointer"); n->type = ty_int; }
        break;
    }

    /* Struct member access s.m */
    case ND_MEMBER: {
        analyze_expr(n->left);
        Type *t = n->left->type;
        if (!t || (t->kind != TY_STRUCT && t->kind != TY_UNION))
            error_at(n->loc, "member access on non-struct/union");
        for (Member *m = t->members; m; m = m->next) {
            if (strcmp(m->name, n->name) == 0) {
                n->type   = m->type;
                n->member = m;
                goto member_done;
            }
        }
        error_at(n->loc, "struct has no member '%s'", n->name);
    member_done:
        break;
    }

    /* Pointer member access p->m */
    case ND_ARROW: {
        analyze_expr(n->left);
        Type *t = n->left->type;
        if (!t || t->kind != TY_PTR)
            error_at(n->loc, "'->' on non-pointer");
        t = t->base;
        if (!t || (t->kind != TY_STRUCT && t->kind != TY_UNION))
            error_at(n->loc, "'->' on pointer to non-struct/union");
        for (Member *m = t->members; m; m = m->next) {
            if (strcmp(m->name, n->name) == 0) {
                n->type   = m->type;
                n->member = m;
                goto arrow_done;
            }
        }
        error_at(n->loc, "struct has no member '%s'", n->name);
    arrow_done:
        break;
    }

    /* Function call */
    case ND_CALL: {
        analyze_expr(n->callee);
        for (int i = 0; i < n->n_args; i++)
            analyze_expr(n->args[i]);

        Type *ft = n->callee->type;
        /* Decay function pointer: &f or just f */
        if (ft && ft->kind == TY_PTR && ft->base && ft->base->kind == TY_FUNC)
            ft = ft->base;
        if (ft && ft->kind == TY_FUNC) {
            n->type = ft->ret;
            /* Type-check fixed arguments */
            Param *pm = ft->params;
            for (int i = 0; i < n->n_args && pm; i++, pm = pm->next) {
                if (n->args[i]->type && pm->type &&
                    n->args[i]->type != pm->type)
                    n->args[i] = make_cast(n->args[i], pm->type);
            }
        } else {
            n->type = ty_int;  /* unknown function — assume int */
        }
        break;
    }

    case ND_INIT_LIST:
        for (int i = 0; i < n->n_stmts; i++)
            analyze_expr(n->stmts[i]);
        n->type = ty_void;
        break;

    default:
        break;
    }
}

/* ============================================================
 * Statement / declaration analysis
 * ============================================================ */

static void analyze_node(Node *n) {
    if (!n) return;

    switch (n->kind) {

    case ND_PROG:
        scope_push();
        for (int i = 0; i < n->n_stmts; i++)
            analyze_node(n->stmts[i]);
        scope_pop();
        /* Validate gotos */
        for (int i = 0; i < n_gotos; i++) {
            bool found = false;
            for (int j = 0; j < n_defined; j++)
                if (strcmp(goto_labels[i], defined_labels[j]) == 0) { found = true; break; }
            if (!found)
                warn_at(goto_locs[i], "goto label '%s' not defined", goto_labels[i]);
        }
        break;

    case ND_FUNC_DEF: {
        /* Register function in current (global) scope */
        Symbol *sym = scope_define(n->name, n->type, false);
        sym->is_global  = true;
        sym->is_static  = n->is_static;
        sym->is_defined = true;
        sym->asm_name   = n->is_static ?
            xstrdup(n->name) :  /* TODO: mangling for static */
            xstrdup(n->name);
        n->sym = sym;

        /* Save / reset per-function state */
        Type *saved_ret = cur_func_ret;
        int   saved_off = cur_stack_off;
        int   saved_max = max_stack_off;
        cur_func_ret  = n->type->ret;
        cur_stack_off = 0;
        max_stack_off = 0;
        n_defined = n_gotos = 0;

        scope_push();
        /* Register parameters */
        for (int i = 0; i < n->n_params; i++) {
            Node *pn = n->params[i];
            pn->type = resolve_type(pn->type);
            if (pn->name) {
                Symbol *ps = scope_define(pn->name, pn->type, false);
                ps->offset = alloc_local(pn->type->size, pn->type->align);
                pn->offset = ps->offset;
                pn->sym    = ps;
            }
        }

        analyze_node(n->body);
        scope_pop();

        /* Store total stack frame size in function node */
        n->offset = (max_stack_off + 15) & ~15;  /* 16-byte align */

        cur_func_ret  = saved_ret;
        cur_stack_off = saved_off;
        max_stack_off = saved_max;
        break;
    }

    case ND_VAR_DECL: {
        n->type = resolve_type(n->type);

        /* typedef declaration */
        if (n->is_extern && n->type) {
            /* is_extern flag re-purposed to mark typedef in parser */
            if (n->name) {
                Symbol *sym = scope_define(n->name, n->type, true);
                n->sym = sym;
            }
            break;
        }

        bool is_global = (cur_scope->parent == NULL);

        if (n->name) {
            Symbol *sym = scope_define(n->name, n->type, false);
            sym->is_global = is_global;
            sym->is_static = n->is_static;
            sym->is_extern = n->is_extern;
            if (is_global || n->is_static) {
                /* Generate assembly label */
                char buf[256];
                if (n->is_static && !is_global)
                    snprintf(buf, sizeof buf, ".LC_static_%s", n->name);
                else
                    snprintf(buf, sizeof buf, "%s", n->name);
                sym->asm_name = xstrdup(buf);
            } else {
                sym->offset = alloc_local(n->type->size ? n->type->size : 8,
                                          n->type->align ? n->type->align : 8);
                n->offset   = sym->offset;
            }
            n->sym = sym;
        }

        if (n->init_expr)
            analyze_expr(n->init_expr);
        break;
    }

    case ND_BLOCK:
        scope_push();
        for (int i = 0; i < n->n_stmts; i++)
            analyze_node(n->stmts[i]);
        scope_pop();
        break;

    case ND_EXPR_STMT:
        analyze_expr(n->left);
        break;

    case ND_IF:
        analyze_expr(n->cond);
        analyze_node(n->then);
        analyze_node(n->els);
        break;

    case ND_WHILE:
        analyze_expr(n->cond);
        loop_depth++;
        analyze_node(n->body);
        loop_depth--;
        break;

    case ND_DO_WHILE:
        loop_depth++;
        analyze_node(n->body);
        loop_depth--;
        analyze_expr(n->cond);
        break;

    case ND_FOR:
        scope_push();
        analyze_node(n->init);
        analyze_expr(n->cond);
        analyze_expr(n->update);
        loop_depth++;
        analyze_node(n->body);
        loop_depth--;
        scope_pop();
        break;

    case ND_SWITCH:
        analyze_expr(n->cond);
        switch_depth++;
        analyze_node(n->body);
        switch_depth--;
        break;

    case ND_CASE:
    case ND_DEFAULT:
        if (switch_depth == 0)
            error_at(n->loc, "case/default outside switch");
        analyze_node(n->body);
        break;

    case ND_RETURN:
        if (n->left) {
            analyze_expr(n->left);
            if (cur_func_ret && n->left->type &&
                n->left->type != cur_func_ret)
                n->left = make_cast(n->left, cur_func_ret);
        }
        break;

    case ND_BREAK:
        if (loop_depth == 0 && switch_depth == 0)
            error_at(n->loc, "break outside loop/switch");
        break;

    case ND_CONTINUE:
        if (loop_depth == 0)
            error_at(n->loc, "continue outside loop");
        break;

    case ND_LABEL:
        if (n_defined < MAX_LABELS)
            defined_labels[n_defined++] = n->name;
        analyze_node(n->body);
        break;

    case ND_GOTO:
        if (n_gotos < MAX_LABELS) {
            goto_labels[n_gotos] = n->name;
            goto_locs[n_gotos]   = n->loc;
            n_gotos++;
        }
        break;

    case ND_NULL_STMT:
        break;

    default:
        /* Could be an expression node at top level (rare) */
        analyze_expr(n);
        break;
    }
}

/* ============================================================
 * Public entry point
 * ============================================================ */

void sema(Node *prog) {
    analyze_node(prog);
}
