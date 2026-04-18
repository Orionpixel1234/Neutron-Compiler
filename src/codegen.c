/*
 * codegen.c — x86-64 code generator for Neutron
 *
 * Target:   Linux x86-64 (System V AMD64 ABI)
 * Output:   Intel syntax NASM-compatible assembly (.asm)
 *
 * Strategy: single-pass AST walk; no IR.
 *   - Integers:  result always in rax (zero/sign-extended as needed).
 *   - Floats:    result always in xmm0.
 *   - Lvalues:   address in rax.
 *   - Binary ops: save left on stack, compute right, combine.
 *   - Calls:     push args in reverse, move first 6 into registers.
 *
 * Register convention (caller-saved temporaries):
 *   rax   = accumulator / integer return value
 *   rcx   = right-hand operand (after pop), shift count
 *   rdx   = used by idiv (quotient remainder), compound assign scratch
 *   rdi rsi rdx rcx r8 r9  = integer argument registers
 *   xmm0  = float accumulator / float return value
 *   xmm1  = float scratch
 *
 * Callee-saved: rbp rbx r12–r15  (only rbp is used here)
 */

#include "../neutron.h"

/* ============================================================
 * Context
 * ============================================================ */

static FILE *OUT;
static int   label_cnt;

static int new_label(void) { return label_cnt++; }

/* ============================================================
 * Emission helpers
 * ============================================================ */

/* Indented instruction */
#define E(...)  fprintf(OUT, "\t" __VA_ARGS__)
/* Unindented (labels, section directives, etc.) */
#define EL(...) fprintf(OUT, __VA_ARGS__)

static void emit_label(int id)        { fprintf(OUT, ".L%d:\n", id); }
static void emit_named(const char *n) { fprintf(OUT, "%s:\n", n); }

/* Integer argument registers (System V AMD64 ABI) */
static const char *argregs_i[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
/* Float argument registers */
static const char *argregs_f[] = {
    "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7"
};

/* ============================================================
 * Per-function frame size
 * ============================================================ */

static int frame_size;

/* ============================================================
 * Forward declarations
 * ============================================================ */

static void gen_expr  (Node *n);
static void gen_lvalue(Node *n);
static void gen_stmt  (Node *n);
static void gen_decl  (Node *n);

/* ============================================================
 * Load value from address-in-rax into rax (or xmm0 for floats)
 * ============================================================ */

static void load(Type *t) {
    if (!t) { E("mov rax, [rax]\n"); return; }

    if (t->kind == TY_FLOAT)  { E("movss xmm0, [rax]\n"); return; }
    if (t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE)
                               { E("movsd xmm0, [rax]\n"); return; }

    switch (t->size) {
    case 1:
        if (ty_is_signed(t)) E("movsx rax, byte [rax]\n");
        else                 E("movzx rax, byte [rax]\n");
        break;
    case 2:
        if (ty_is_signed(t)) E("movsx rax, word [rax]\n");
        else                 E("movzx rax, word [rax]\n");
        break;
    case 4:
        if (ty_is_signed(t)) E("movsxd rax, dword [rax]\n");
        else                 E("mov eax, [rax]\n");  /* zero-extends to rax */
        break;
    default:
        E("mov rax, [rax]\n");
    }
}

/* ============================================================
 * Store rax (int) or xmm0 (float) to address-in-rcx
 * ============================================================ */

static void store(Type *t) {
    if (!t) { E("mov [rcx], rax\n"); return; }

    if (t->kind == TY_FLOAT)  { E("movss [rcx], xmm0\n"); return; }
    if (t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE)
                               { E("movsd [rcx], xmm0\n"); return; }

    switch (t->size) {
    case 1: E("mov byte  [rcx], al\n");   break;
    case 2: E("mov word  [rcx], ax\n");   break;
    case 4: E("mov dword [rcx], eax\n");  break;
    default: E("mov [rcx], rax\n");       break;
    }
}

/* ============================================================
 * Sign/zero-extend rax to match a narrower type
 * ============================================================ */

static void extend_rax(Type *t) {
    if (!t) return;
    switch (t->kind) {
    case TY_CHAR:   E("movsx  rax, al\n");    break;
    case TY_UCHAR:  E("movzx  rax, al\n");    break;
    case TY_SHORT:  E("movsx  rax, ax\n");    break;
    case TY_USHORT: E("movzx  rax, ax\n");    break;
    case TY_INT:    E("movsxd rax, eax\n");   break;
    default: break;
    }
}

/* ============================================================
 * Lvalue codegen — leaves address in rax
 * ============================================================ */

static void gen_lvalue(Node *n) {
    switch (n->kind) {
    case ND_IDENT:
        if (!n->sym) error_at(n->loc, "unresolved symbol '%s'", n->name);
        if (n->sym->is_global || n->sym->is_static)
            E("lea rax, [%s]\n", n->sym->asm_name);
        else
            E("lea rax, [rbp%+d]\n", n->sym->offset);
        break;

    case ND_DEREF:
        gen_expr(n->left);   /* pointer value → rax */
        break;

    case ND_INDEX: {
        gen_lvalue(n->left);
        E("push rax\n");
        gen_expr(n->right);
        int elem_sz = n->type ? n->type->size : 8;
        if (elem_sz != 1) E("imul rax, %d\n", elem_sz);
        E("pop rcx\n");
        E("add rax, rcx\n");
        break;
    }

    case ND_MEMBER:
        gen_lvalue(n->left);
        if (n->member) E("add rax, %d\n", n->member->offset);
        break;

    case ND_ARROW:
        gen_expr(n->left);
        if (n->member) E("add rax, %d\n", n->member->offset);
        break;

    default:
        error_at(n->loc, "not an lvalue (kind=%d)", n->kind);
    }
}

/* ============================================================
 * Expression codegen
 * Result: integer → rax,  float → xmm0
 * ============================================================ */

static void gen_expr(Node *n) {
    if (!n) return;

    switch (n->kind) {

    /* ---- Literals ---- */
    case ND_INT_LIT:
        E("mov rax, %lld\n", (long long)n->ival);
        return;

    case ND_FLOAT_LIT:
        if (n->type && n->type->kind == TY_FLOAT) {
            union { float f; uint32_t u; } v;
            v.f = (float)n->fval;
            E("mov eax, 0x%08X\n", v.u);
            E("movd xmm0, eax\n");
        } else {
            union { double d; uint64_t u; } v;
            v.d = (double)n->fval;
            E("mov rax, 0x%016llX\n", (unsigned long long)v.u);
            E("movq xmm0, rax\n");
        }
        return;

    case ND_STR_LIT:
        E("lea rax, [.Lstr%d]\n", n->str_id);
        return;

    /* ---- Identifier (rvalue) ---- */
    case ND_IDENT:
        gen_lvalue(n);
        load(n->type);
        return;

    /* ---- Address-of ---- */
    case ND_ADDR:
        gen_lvalue(n->left);
        return;

    /* ---- Dereference ---- */
    case ND_DEREF:
        gen_expr(n->left);
        load(n->type);
        return;

    /* ---- Struct member / array subscript (rvalue) ---- */
    case ND_MEMBER:
    case ND_ARROW:
    case ND_INDEX:
        gen_lvalue(n);
        load(n->type);
        return;

    /* ---- Unary minus ---- */
    case ND_NEG:
        gen_expr(n->left);
        if (n->type && ty_is_float(n->type)) {
            if (n->type->kind == TY_FLOAT) {
                E("mov eax, 0x80000000\n");
                E("movd xmm1, eax\n");
                E("xorps xmm0, xmm1\n");
            } else {
                E("mov rax, 0x8000000000000000\n");
                E("movq xmm1, rax\n");
                E("xorpd xmm0, xmm1\n");
            }
        } else {
            E("neg rax\n");
        }
        return;

    /* ---- Logical not ---- */
    case ND_NOT:
        gen_expr(n->left);
        E("cmp rax, 0\n");
        E("sete al\n");
        E("movzx rax, al\n");
        return;

    /* ---- Bitwise not ---- */
    case ND_BITNOT:
        gen_expr(n->left);
        E("not rax\n");
        return;

    /* ---- Cast ---- */
    case ND_CAST: {
        gen_expr(n->left);
        Type *from = n->left->type;
        Type *to   = n->type;
        if (!from || !to) return;

        if (ty_is_float(from) && ty_is_integer(to)) {
            if (from->kind == TY_FLOAT) E("cvttss2si rax, xmm0\n");
            else                        E("cvttsd2si rax, xmm0\n");
            return;
        }
        if (ty_is_integer(from) && ty_is_float(to)) {
            if (to->kind == TY_FLOAT) E("cvtsi2ss xmm0, rax\n");
            else                      E("cvtsi2sd xmm0, rax\n");
            return;
        }
        if (ty_is_float(from) && ty_is_float(to)) {
            if (from->kind == TY_FLOAT && to->kind != TY_FLOAT)
                E("cvtss2sd xmm0, xmm0\n");
            else if (from->kind != TY_FLOAT && to->kind == TY_FLOAT)
                E("cvtsd2ss xmm0, xmm0\n");
            return;
        }
        extend_rax(to);
        return;
    }

    /* ---- Pre-increment / decrement ---- */
    case ND_PRE_INC:
    case ND_PRE_DEC: {
        int delta = 1;
        if (n->left->type && ty_is_pointer(n->left->type))
            delta = n->left->type->base ? n->left->type->base->size : 1;
        gen_lvalue(n->left);
        E("mov rcx, rax\n");   /* save address */
        load(n->left->type);
        if (n->kind == ND_PRE_INC) E("add rax, %d\n", delta);
        else                       E("sub rax, %d\n", delta);
        store(n->left->type);
        E("mov rax, rcx\n");
        load(n->left->type);
        return;
    }

    /* ---- Post-increment / decrement ---- */
    case ND_POST_INC:
    case ND_POST_DEC: {
        int delta = 1;
        if (n->left->type && ty_is_pointer(n->left->type))
            delta = n->left->type->base ? n->left->type->base->size : 1;
        gen_lvalue(n->left);
        E("mov rcx, rax\n");
        load(n->left->type);
        E("push rax\n");       /* save old value */
        if (n->kind == ND_POST_INC) E("add rax, %d\n", delta);
        else                        E("sub rax, %d\n", delta);
        store(n->left->type);
        E("pop rcx\n");
        E("mov rax, rcx\n");   /* return old value */
        return;
    }

    /* ---- Assignment ---- */
    case ND_ASSIGN: {
        gen_lvalue(n->left);
        E("push rax\n");        /* save lvalue address */
        gen_expr(n->right);
        E("pop rcx\n");         /* restore lvalue address → rcx */
        store(n->left->type);
        E("mov rax, rcx\n");
        load(n->left->type);
        return;
    }

    /* ---- Compound assignment (+=, -=, ...) ---- */
    case ND_ASSIGN_OP: {
        gen_lvalue(n->left);
        E("push rax\n");        /* [1] save lvalue address */
        load(n->left->type);
        E("push rax\n");        /* [2] save old value */
        gen_expr(n->right);
        E("pop rcx\n");         /* [2] old value → rcx;  rax = RHS */
        E("xchg rax, rcx\n");   /*     rax = old value,  rcx = RHS */

        switch (n->op) {
        case TK_PLUS_EQ:    E("add  rax, rcx\n");  break;
        case TK_MINUS_EQ:   E("sub  rax, rcx\n");  break;
        case TK_STAR_EQ:    E("imul rax, rcx\n");  break;
        case TK_SLASH_EQ:   E("cqo\n"); E("idiv rcx\n"); break;
        case TK_PERCENT_EQ: E("cqo\n"); E("idiv rcx\n"); E("mov rax, rdx\n"); break;
        case TK_AMP_EQ:     E("and  rax, rcx\n");  break;
        case TK_PIPE_EQ:    E("or   rax, rcx\n");  break;
        case TK_CARET_EQ:   E("xor  rax, rcx\n");  break;
        /* shift: count must be in cl */
        case TK_LSHIFT_EQ:  E("shl  rax, cl\n");   break;
        case TK_RSHIFT_EQ:
            if (n->left->type && ty_is_signed(n->left->type))
                             E("sar  rax, cl\n");
            else             E("shr  rax, cl\n");
            break;
        default: break;
        }

        E("pop rcx\n");         /* [1] lvalue address */
        store(n->left->type);
        E("mov rax, rcx\n");
        load(n->left->type);
        return;
    }

    /* ---- Binary arithmetic / bitwise ---- */
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_SHL: case ND_SHR: case ND_BITAND: case ND_BITXOR: case ND_BITOR: {
        bool is_flt = n->type && ty_is_float(n->type);

        /* Evaluate left; save it */
        gen_expr(n->left);
        if (is_flt) { E("sub rsp, 8\n"); E("movsd [rsp], xmm0\n"); }
        else        { E("push rax\n"); }

        /* Evaluate right */
        gen_expr(n->right);

        if (is_flt) {
            /* xmm1 = left,  xmm0 = right */
            E("movsd xmm1, [rsp]\n");
            E("add rsp, 8\n");
            bool single = (n->type->kind == TY_FLOAT);
            switch (n->kind) {
            case ND_ADD: fprintf(OUT, "\t%s xmm1, xmm0\n", single ? "addss" : "addsd"); break;
            case ND_SUB: fprintf(OUT, "\t%s xmm1, xmm0\n", single ? "subss" : "subsd"); break;
            case ND_MUL: fprintf(OUT, "\t%s xmm1, xmm0\n", single ? "mulss" : "mulsd"); break;
            case ND_DIV: fprintf(OUT, "\t%s xmm1, xmm0\n", single ? "divss" : "divsd"); break;
            default: break;
            }
            E("movapd xmm0, xmm1\n");
            return;
        }

        /* rcx = left,  rax = right */
        E("pop rcx\n");

        switch (n->kind) {
        case ND_ADD:
            E("add rax, rcx\n");
            break;
        case ND_SUB:
            /* left - right = rcx - rax */
            E("sub rcx, rax\n");
            E("mov rax, rcx\n");
            break;
        case ND_MUL:
            E("imul rax, rcx\n");
            break;
        case ND_DIV:
            /* rax = rcx / rax */
            E("xchg rax, rcx\n");
            E("cqo\n");
            E("idiv rcx\n");
            break;
        case ND_MOD:
            E("xchg rax, rcx\n");
            E("cqo\n");
            E("idiv rcx\n");
            E("mov rax, rdx\n");
            break;
        case ND_SHL:
            /* rax = rcx << rax  →  swap, put count in cl */
            E("xchg rax, rcx\n");
            E("shl rax, cl\n");
            break;
        case ND_SHR:
            E("xchg rax, rcx\n");
            if (n->left->type && ty_is_signed(n->left->type))
                E("sar rax, cl\n");
            else
                E("shr rax, cl\n");
            break;
        case ND_BITAND: E("and rax, rcx\n"); break;
        case ND_BITXOR: E("xor rax, rcx\n"); break;
        case ND_BITOR:  E("or  rax, rcx\n"); break;
        default: break;
        }
        return;
    }

    /* ---- Comparisons ---- */
    case ND_EQ: case ND_NEQ:
    case ND_LT: case ND_GT: case ND_LEQ: case ND_GEQ: {
        bool is_flt = n->left->type && ty_is_float(n->left->type);

        gen_expr(n->left);
        if (is_flt) { E("sub rsp, 8\n"); E("movsd [rsp], xmm0\n"); }
        else        { E("push rax\n"); }

        gen_expr(n->right);

        if (is_flt) {
            E("movsd xmm1, [rsp]\n");
            E("add rsp, 8\n");
            E("ucomisd xmm1, xmm0\n");  /* xmm1 = left,  xmm0 = right */
        } else {
            E("pop rcx\n");
            E("cmp rcx, rax\n");         /* rcx = left,  rax = right */
        }

        const char *setcc = "sete";
        switch (n->kind) {
        case ND_EQ:  setcc = "sete";  break;
        case ND_NEQ: setcc = "setne"; break;
        case ND_LT:  setcc = "setl";  break;
        case ND_GT:  setcc = "setg";  break;
        case ND_LEQ: setcc = "setle"; break;
        case ND_GEQ: setcc = "setge"; break;
        default: break;
        }
        E("%s al\n", setcc);
        E("movzx rax, al\n");
        return;
    }

    /* ---- Logical && ---- */
    case ND_LOGAND: {
        int lfalse = new_label(), lend = new_label();
        gen_expr(n->left);
        E("cmp rax, 0\n");
        E("je  .L%d\n", lfalse);
        gen_expr(n->right);
        E("cmp rax, 0\n");
        E("je  .L%d\n", lfalse);
        E("mov rax, 1\n");
        E("jmp .L%d\n", lend);
        emit_label(lfalse);
        E("mov rax, 0\n");
        emit_label(lend);
        return;
    }

    /* ---- Logical || ---- */
    case ND_LOGOR: {
        int ltrue = new_label(), lend = new_label();
        gen_expr(n->left);
        E("cmp rax, 0\n");
        E("jne .L%d\n", ltrue);
        gen_expr(n->right);
        E("cmp rax, 0\n");
        E("jne .L%d\n", ltrue);
        E("mov rax, 0\n");
        E("jmp .L%d\n", lend);
        emit_label(ltrue);
        E("mov rax, 1\n");
        emit_label(lend);
        return;
    }

    /* ---- Ternary ?: ---- */
    case ND_TERNARY: {
        int lelse = new_label(), lend = new_label();
        gen_expr(n->cond);
        E("cmp rax, 0\n");
        E("je  .L%d\n", lelse);
        gen_expr(n->then);
        E("jmp .L%d\n", lend);
        emit_label(lelse);
        gen_expr(n->els);
        emit_label(lend);
        return;
    }

    /* ---- Comma ---- */
    case ND_COMMA:
        gen_expr(n->left);
        gen_expr(n->right);
        return;

    /* ---- Function call ---- */
    case ND_CALL: {
        int n_args = n->n_args;

        /* Push all args right-to-left */
        for (int i = n_args - 1; i >= 0; i--) {
            Node *arg = n->args[i];
            gen_expr(arg);
            if (arg->type && ty_is_float(arg->type)) {
                E("sub rsp, 8\n");
                E("movsd [rsp], xmm0\n");
            } else {
                E("push rax\n");
            }
        }

        /* Move first 6 args from stack into registers */
        int i_reg = 0, f_reg = 0;
        for (int i = 0; i < n_args && i < 6; i++) {
            Node *arg = n->args[i];
            if (arg->type && ty_is_float(arg->type)) {
                if (f_reg < 8) E("movsd %s, [rsp]\n", argregs_f[f_reg++]);
                E("add rsp, 8\n");
            } else {
                if (i_reg < 6) E("mov %s, [rsp]\n", argregs_i[i_reg++]);
                E("add rsp, 8\n");
            }
        }

        /* Align stack to 16 bytes before call */
        E("and rsp, -16\n");

        /* rax = number of float args (required for variadic functions) */
        E("mov rax, %d\n", f_reg);

        if (n->callee->kind == ND_IDENT && n->callee->sym)
            E("call %s\n", n->callee->sym->asm_name);
        else {
            gen_expr(n->callee);
            E("call rax\n");
        }

        /* Pop any stack-only args (beyond first 6) */
        int extra = n_args > 6 ? (n_args - 6) : 0;
        if (extra > 0) E("add rsp, %d\n", extra * 8);
        return;
    }

    default:
        warn_at(n->loc, "codegen: unhandled expression kind %d", n->kind);
        E("mov rax, 0\n");
        return;
    }
}

/* ============================================================
 * Statement codegen
 * ============================================================ */

/* Break / continue label stacks */
#define STACK_DEPTH 64
static int break_labels[STACK_DEPTH],    break_top    = 0;
static int continue_labels[STACK_DEPTH], continue_top = 0;

static void push_break   (int l) { if (break_top    < STACK_DEPTH) break_labels[break_top++]       = l; }
static void push_continue(int l) { if (continue_top < STACK_DEPTH) continue_labels[continue_top++] = l; }
static void pop_break    (void)  { if (break_top    > 0) break_top--;    }
static void pop_continue (void)  { if (continue_top > 0) continue_top--; }
static int  cur_break    (void)  { return break_top    > 0 ? break_labels[break_top-1]       : -1; }
static int  cur_continue (void)  { return continue_top > 0 ? continue_labels[continue_top-1] : -1; }

static void gen_stmt(Node *n) {
    if (!n) return;

    switch (n->kind) {

    case ND_NULL_STMT:
        return;

    case ND_BLOCK:
        for (int i = 0; i < n->n_stmts; i++) {
            Node *s = n->stmts[i];
            if (s->kind == ND_VAR_DECL || s->kind == ND_FUNC_DEF)
                gen_decl(s);
            else
                gen_stmt(s);
        }
        return;

    case ND_EXPR_STMT:
        gen_expr(n->left);
        return;

    case ND_IF: {
        int lelse = new_label(), lend = new_label();
        gen_expr(n->cond);
        E("cmp rax, 0\n");
        E("je  .L%d\n", lelse);
        gen_stmt(n->then);
        E("jmp .L%d\n", lend);
        emit_label(lelse);
        if (n->els) gen_stmt(n->els);
        emit_label(lend);
        return;
    }

    case ND_WHILE: {
        int lcond = new_label(), lend = new_label();
        push_break(lend); push_continue(lcond);
        emit_label(lcond);
        gen_expr(n->cond);
        E("cmp rax, 0\n");
        E("je  .L%d\n", lend);
        gen_stmt(n->body);
        E("jmp .L%d\n", lcond);
        emit_label(lend);
        pop_break(); pop_continue();
        return;
    }

    case ND_DO_WHILE: {
        int lbody = new_label(), lend = new_label();
        push_break(lend); push_continue(lbody);
        emit_label(lbody);
        gen_stmt(n->body);
        gen_expr(n->cond);
        E("cmp rax, 0\n");
        E("jne .L%d\n", lbody);
        emit_label(lend);
        pop_break(); pop_continue();
        return;
    }

    case ND_FOR: {
        int lcond = new_label(), lupdate = new_label(), lend = new_label();
        push_break(lend); push_continue(lupdate);
        if (n->init) {
            if (n->init->kind == ND_VAR_DECL || n->init->kind == ND_BLOCK)
                gen_decl(n->init);
            else
                gen_stmt(n->init);
        }
        emit_label(lcond);
        if (n->cond) {
            gen_expr(n->cond);
            E("cmp rax, 0\n");
            E("je  .L%d\n", lend);
        }
        gen_stmt(n->body);
        emit_label(lupdate);
        if (n->update) gen_expr(n->update);
        E("jmp .L%d\n", lcond);
        emit_label(lend);
        pop_break(); pop_continue();
        return;
    }

    case ND_SWITCH: {
        int lend = new_label();
        push_break(lend);

        gen_expr(n->cond);
        E("mov r10, rax\n");    /* save switch value in r10 */

        /* Collect case values from direct children of body block */
        typedef struct { long long val; int lbl; } CaseEntry;
        CaseEntry cases[256];
        int n_cases = 0;
        int default_lbl = lend;

        Node *body = n->body;
        if (body && body->kind == ND_BLOCK) {
            for (int i = 0; i < body->n_stmts; i++) {
                Node *s = body->stmts[i];
                if (s->kind == ND_CASE && n_cases < 256) {
                    cases[n_cases].val = s->case_val;
                    cases[n_cases].lbl = new_label();
                    s->str_id = cases[n_cases].lbl;
                    n_cases++;
                } else if (s->kind == ND_DEFAULT) {
                    default_lbl = new_label();
                    s->str_id = default_lbl;
                }
            }
        }

        /* Emit comparison chain */
        for (int i = 0; i < n_cases; i++) {
            E("cmp r10, %lld\n", cases[i].val);
            E("je  .L%d\n", cases[i].lbl);
        }
        E("jmp .L%d\n", default_lbl);

        gen_stmt(n->body);
        emit_label(lend);
        pop_break();
        return;
    }

    case ND_CASE:
        emit_label(n->str_id);
        gen_stmt(n->body);
        return;

    case ND_DEFAULT:
        emit_label(n->str_id);
        gen_stmt(n->body);
        return;

    case ND_RETURN:
        if (n->left) gen_expr(n->left);
        E("mov rsp, rbp\n");
        E("pop rbp\n");
        E("ret\n");
        return;

    case ND_BREAK: {
        int lbl = cur_break();
        if (lbl >= 0) E("jmp .L%d\n", lbl);
        return;
    }

    case ND_CONTINUE: {
        int lbl = cur_continue();
        if (lbl >= 0) E("jmp .L%d\n", lbl);
        return;
    }

    case ND_LABEL:
        EL("%s:\n", n->name);
        gen_stmt(n->body);
        return;

    case ND_GOTO:
        E("jmp %s\n", n->name);
        return;

    case ND_VAR_DECL:
    case ND_FUNC_DEF:
        gen_decl(n);
        return;

    default:
        warn_at(n->loc, "codegen: unhandled statement kind %d", n->kind);
        return;
    }
}

/* ============================================================
 * Global initializer (compile-time constants only)
 * ============================================================ */

static void gen_global_init(Node *n, Type *type, const char *sym_name);

static void gen_global_init(Node *n, Type *type, const char *sym_name) {
    (void)sym_name;
    switch (n->kind) {
    case ND_INT_LIT: {
        int sz = type ? type->size : 8;
        if      (sz == 1) EL("\tdb %lld\n",  n->ival);
        else if (sz == 2) EL("\tdw %lld\n",  n->ival);
        else if (sz == 4) EL("\tdd %lld\n",  n->ival);
        else              EL("\tdq %lld\n",  n->ival);
        return;
    }
    case ND_FLOAT_LIT: {
        if (type && type->kind == TY_FLOAT) {
            union { float f; uint32_t u; } v; v.f = (float)n->fval;
            EL("\tdd 0x%08X\n", v.u);
        } else {
            union { double d; uint64_t u; } v; v.d = (double)n->fval;
            EL("\tdq 0x%016llX\n", (unsigned long long)v.u);
        }
        return;
    }
    case ND_STR_LIT:
        EL("\tdq .Lstr%d\n", n->str_id);
        return;
    case ND_INIT_LIST:
        if (type && (type->kind == TY_STRUCT || type->kind == TY_UNION)) {
            Member *m = type->members;
            for (int i = 0; i < n->n_stmts && m; i++, m = m->next)
                gen_global_init(n->stmts[i], m->type, NULL);
        } else if (type && type->kind == TY_ARRAY) {
            for (int i = 0; i < n->n_stmts; i++)
                gen_global_init(n->stmts[i], type->base, NULL);
        }
        return;
    default:
        EL("\ttimes %d db 0\n", type ? type->size : 8);
        return;
    }
}

/* ============================================================
 * Declaration codegen
 * ============================================================ */

static void gen_decl(Node *n) {
    if (!n) return;

    switch (n->kind) {

    case ND_FUNC_DEF: {
        if (!n->sym) return;

        if (!n->is_static) EL("global %s\n", n->sym->asm_name);
        EL("section .text\n");
        emit_named(n->sym->asm_name);

        /* Prologue */
        E("push rbp\n");
        E("mov  rbp, rsp\n");
        frame_size = n->offset;
        if (frame_size > 0) E("sub rsp, %d\n", frame_size);

        /* Copy register arguments to their stack homes */
        int i_reg = 0, f_reg = 0;
        for (int i = 0; i < n->n_params; i++) {
            Node *pn = n->params[i];
            if (!pn->type) continue;
            if (ty_is_float(pn->type)) {
                if (f_reg < 8)
                    E("movsd [rbp%+d], %s\n", pn->offset, argregs_f[f_reg++]);
            } else {
                if (i_reg < 6)
                    E("mov [rbp%+d], %s\n", pn->offset, argregs_i[i_reg++]);
            }
        }

        /* Body */
        gen_stmt(n->body);

        /* Default return 0 (reachable when function has no explicit return) */
        E("mov rax, 0\n");
        E("mov rsp, rbp\n");
        E("pop rbp\n");
        E("ret\n");
        return;
    }

    case ND_VAR_DECL: {
        if (!n->sym) return;
        /* Skip function prototypes and extern declarations */
        if (n->type && n->type->kind == TY_FUNC) return;
        if (n->sym->is_extern) return;

        if (!n->sym->is_global && !n->sym->is_static) {
            /* Local variable */
            if (n->init_expr) {
                gen_expr(n->init_expr);
                E("lea rcx, [rbp%+d]\n", n->sym->offset);
                store(n->type);
            } else if (n->type && n->type->size > 0) {
                /* Zero-initialise */
                E("lea rdi, [rbp%+d]\n", n->sym->offset);
                E("mov rax, 0\n");
                for (int b = 0; b < n->type->size; b += 8) {
                    int rem = n->type->size - b;
                    if      (rem >= 8) E("mov qword [rdi+%d], rax\n", b);
                    else if (rem >= 4) E("mov dword [rdi+%d], eax\n", b);
                    else if (rem >= 2) E("mov word  [rdi+%d], ax\n",  b);
                    else               E("mov byte  [rdi+%d], al\n",  b);
                }
            }
            return;
        }

        /* Global / static variable */
        EL("section .data\n");
        if (!n->sym->is_static) EL("global %s\n", n->sym->asm_name);
        EL("align %d\n", n->type ? n->type->align : 8);
        emit_named(n->sym->asm_name);

        if (!n->init_expr)
            EL("\ttimes %d db 0\n", n->type ? n->type->size : 8);
        else
            gen_global_init(n->init_expr, n->type, n->sym->asm_name);
        return;
    }

    case ND_BLOCK:
        for (int i = 0; i < n->n_stmts; i++)
            gen_decl(n->stmts[i]);
        return;

    default:
        gen_stmt(n);
        return;
    }
}

/* ============================================================
 * Public entry point
 * ============================================================ */

/* ============================================================
 * Pre-pass: collect extern symbols (called but not defined here)
 * ============================================================ */

#define MAX_EXTERNS 256
static const char *extern_syms[MAX_EXTERNS];
static int         n_extern_syms = 0;

static void collect_externs_expr(Node *n);
static void collect_externs_node(Node *n);

static void collect_externs_expr(Node *n) {
    if (!n) return;
    if (n->kind == ND_CALL) {
        if (n->callee && n->callee->kind == ND_IDENT && n->callee->sym) {
            Symbol *s = n->callee->sym;
            if (s->is_extern || !s->is_defined) {
                /* Add to extern list if not already present */
                bool found = false;
                for (int i = 0; i < n_extern_syms; i++)
                    if (strcmp(extern_syms[i], s->asm_name ? s->asm_name : s->name) == 0)
                        { found = true; break; }
                if (!found && n_extern_syms < MAX_EXTERNS)
                    extern_syms[n_extern_syms++] = s->asm_name ? s->asm_name : s->name;
            }
        }
        collect_externs_expr(n->callee);
        for (int i = 0; i < n->n_args; i++) collect_externs_expr(n->args[i]);
        return;
    }
    collect_externs_expr(n->left);
    collect_externs_expr(n->right);
    collect_externs_expr(n->cond);
    collect_externs_expr(n->then);
    collect_externs_expr(n->els);
    collect_externs_expr(n->init_expr);
    for (int i = 0; i < n->n_args;   i++) collect_externs_expr(n->args[i]);
    for (int i = 0; i < n->n_stmts;  i++) collect_externs_node(n->stmts[i]);
}

static void collect_externs_node(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_FUNC_DEF:
        collect_externs_node(n->body);
        break;
    case ND_VAR_DECL:
        collect_externs_expr(n->init_expr);
        break;
    case ND_BLOCK:
        for (int i = 0; i < n->n_stmts; i++) collect_externs_node(n->stmts[i]);
        break;
    case ND_EXPR_STMT:
        collect_externs_expr(n->left);
        break;
    case ND_IF:
        collect_externs_expr(n->cond);
        collect_externs_node(n->then);
        collect_externs_node(n->els);
        break;
    case ND_WHILE: case ND_DO_WHILE:
        collect_externs_expr(n->cond);
        collect_externs_node(n->body);
        break;
    case ND_FOR:
        collect_externs_node(n->init);
        collect_externs_expr(n->cond);
        collect_externs_expr(n->update);
        collect_externs_node(n->body);
        break;
    case ND_SWITCH:
        collect_externs_expr(n->cond);
        collect_externs_node(n->body);
        break;
    case ND_RETURN: case ND_LABEL: case ND_CASE: case ND_DEFAULT:
        collect_externs_expr(n->left);
        collect_externs_node(n->body);
        break;
    default:
        collect_externs_expr(n);
        break;
    }
}

/* ============================================================
 * Public entry point
 * ============================================================ */

void codegen(Node *prog, FILE *out) {
    OUT       = out;
    label_cnt = 0;

    /* Collect extern symbols used by this translation unit */
    n_extern_syms = 0;
    for (int i = 0; i < prog->n_stmts; i++)
        collect_externs_node(prog->stmts[i]);

    /* File header */
    EL("; Generated by Neutron Compiler\n");
    EL("[bits 64]\n");
    EL("[default rel]\n\n");   /* all bare label refs become RIP-relative */

    /* Extern declarations (for linker) */
    for (int i = 0; i < n_extern_syms; i++)
        EL("extern %s\n", extern_syms[i]);
    if (n_extern_syms > 0) EL("\n");

    /* Top-level declarations */
    for (int i = 0; i < prog->n_stmts; i++)
        gen_decl(prog->stmts[i]);

    /* String literal table */
    if (str_count > 0) {
        EL("section .rodata\n");
        for (int i = 0; i < str_count; i++) {
            EL(".Lstr%d:\n", i);
            EL("\tdb \"");
            for (int j = 0; j < str_table[i].len - 1; j++) {
                unsigned char c = (unsigned char)str_table[i].data[j];
                if      (c == '"')  EL("\\\"");
                else if (c == '\\') EL("\\\\");
                else if (c == '\n') EL("\", 10, \"");
                else if (c == '\t') EL("\", 9, \"");
                else if (c == '\r') EL("\", 13, \"");
                else if (c < 32 || c > 126) EL("\", %d, \"", c);
                else EL("%c", c);
            }
            EL("\", 0\n");
        }
    }
}
