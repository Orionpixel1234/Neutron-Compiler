/*
 * codegen.c — x86 code generator for Neutron
 *
 * 64-bit target: Linux x86-64  (System V AMD64 ABI)
 * 32-bit target: Linux i386    (cdecl ABI)  — enabled by mode_32bit flag
 *
 * 64-bit calling convention: first 6 int args in rdi/rsi/rdx/rcx/r8/r9
 * 32-bit calling convention: all args pushed right-to-left (cdecl), caller cleans
 *
 * Integer result: rax / eax
 * Float result:   xmm0  (SSE used in both modes)
 * Lvalue address: rax / eax
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
 * Mode-dependent register names — set once in codegen()
 * ============================================================ */

static bool        m32;    /* copy of mode_32bit */
static const char *Rax;   /* "rax"/"eax"  — accumulator        */
static const char *Rcx;   /* "rcx"/"ecx"  — right-hand / shift */
static const char *Rdx;   /* "rdx"/"edx"  — idiv remainder     */
static const char *Rbp;   /* "rbp"/"ebp"  — frame pointer      */
static const char *Rsp;   /* "rsp"/"esp"  — stack pointer      */
static const char *Rdi;   /* "rdi"/"edi"  — zero-init scratch  */
static const char *Rsw;   /* switch temp: "r10" or "esi"       */
static int         Psize; /* pointer size in bytes: 8 or 4     */

static void init_mode(void) {
    m32   = mode_32bit;
    Rax   = m32 ? "eax" : "rax";
    Rcx   = m32 ? "ecx" : "rcx";
    Rdx   = m32 ? "edx" : "rdx";
    Rbp   = m32 ? "ebp" : "rbp";
    Rsp   = m32 ? "esp" : "rsp";
    Rdi   = m32 ? "edi" : "rdi";
    Rsw   = m32 ? "esi" : "r10";
    Psize = m32 ? 4 : 8;
}

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
    if (!t) { E("mov %s, [%s]\n", Rax, Rax); return; }

    if (t->kind == TY_FLOAT)
        { E("movss xmm0, [%s]\n", Rax); return; }
    if (t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE)
        { E("movsd xmm0, [%s]\n", Rax); return; }

    switch (t->size) {
    case 1:
        if (ty_is_signed(t)) E("movsx %s, byte [%s]\n", Rax, Rax);
        else                 E("movzx %s, byte [%s]\n", Rax, Rax);
        break;
    case 2:
        if (ty_is_signed(t)) E("movsx %s, word [%s]\n", Rax, Rax);
        else                 E("movzx %s, word [%s]\n", Rax, Rax);
        break;
    case 4:
        if (!m32 && ty_is_signed(t)) E("movsxd rax, dword [rax]\n");
        else                         E("mov %s, [%s]\n", Rax, Rax);
        break;
    default:
        E("mov %s, [%s]\n", Rax, Rax);
    }
}

/* ============================================================
 * Store rax (int) or xmm0 (float) to address-in-rcx
 * ============================================================ */

static void store(Type *t) {
    if (!t) { E("mov [%s], %s\n", Rcx, Rax); return; }

    if (t->kind == TY_FLOAT)
        { E("movss [%s], xmm0\n", Rcx); return; }
    if (t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE)
        { E("movsd [%s], xmm0\n", Rcx); return; }

    switch (t->size) {
    case 1: E("mov byte  [%s], al\n",  Rcx); break;
    case 2: E("mov word  [%s], ax\n",  Rcx); break;
    case 4: E("mov dword [%s], eax\n", Rcx); break;
    default: E("mov [%s], %s\n", Rcx, Rax); break;
    }
}

/* ============================================================
 * Sign/zero-extend rax to match a narrower type
 * ============================================================ */

static void extend_rax(Type *t) {
    if (!t) return;
    switch (t->kind) {
    case TY_CHAR:   E("movsx  %s, al\n", Rax); break;
    case TY_UCHAR:  E("movzx  %s, al\n", Rax); break;
    case TY_SHORT:  E("movsx  %s, ax\n", Rax); break;
    case TY_USHORT: E("movzx  %s, ax\n", Rax); break;
    case TY_INT:    if (!m32) E("movsxd rax, eax\n"); break;
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
        if (n->sym->is_global || n->sym->is_static) {
            if (m32) E("mov %s, %s\n",         Rax, n->sym->asm_name);
            else     E("lea %s, [%s]\n",        Rax, n->sym->asm_name);
        } else {
            if (m32) {
                E("mov %s, %s\n", Rax, Rbp);
                if (n->sym->offset) E("add %s, %d\n", Rax, n->sym->offset);
            } else {
                E("lea %s, [%s%+d]\n", Rax, Rbp, n->sym->offset);
            }
        }
        break;

    case ND_DEREF:
        gen_expr(n->left);
        break;

    case ND_INDEX: {
        gen_lvalue(n->left);
        E("push %s\n", Rax);
        gen_expr(n->right);
        int elem_sz = n->type ? n->type->size : Psize;
        if (elem_sz != 1) E("imul %s, %d\n", Rax, elem_sz);
        E("pop %s\n", Rcx);
        E("add %s, %s\n", Rax, Rcx);
        break;
    }

    case ND_MEMBER:
        gen_lvalue(n->left);
        if (n->member) E("add %s, %d\n", Rax, n->member->offset);
        break;

    case ND_ARROW:
        gen_expr(n->left);
        if (n->member) E("add %s, %d\n", Rax, n->member->offset);
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
        E("mov %s, %lld\n", Rax, (long long)n->ival);
        return;

    case ND_FLOAT_LIT:
        if (n->type && n->type->kind == TY_FLOAT) {
            union { float f; uint32_t u; } v;
            v.f = (float)n->fval;
            E("mov eax, 0x%08X\n", v.u);
            E("movd xmm0, eax\n");
        } else if (m32) {
            /* 32-bit: load 8-byte double via stack */
            union { double d; struct { uint32_t lo, hi; } p; } v;
            v.d = (double)n->fval;
            E("sub %s, 8\n", Rsp);
            E("mov dword [%s],   0x%08X\n", Rsp, v.p.lo);
            E("mov dword [%s+4], 0x%08X\n", Rsp, v.p.hi);
            E("movsd xmm0, [%s]\n", Rsp);
            E("add %s, 8\n", Rsp);
        } else {
            union { double d; uint64_t u; } v;
            v.d = (double)n->fval;
            E("mov rax, 0x%016llX\n", (unsigned long long)v.u);
            E("movq xmm0, rax\n");
        }
        return;

    case ND_STR_LIT:
        if (m32) E("mov %s, .Lstr%d\n", Rax, n->str_id);
        else     E("lea %s, [.Lstr%d]\n", Rax, n->str_id);
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
            } else if (m32) {
                E("sub %s, 8\n", Rsp);
                E("mov dword [%s],   0x00000000\n", Rsp);
                E("mov dword [%s+4], 0x80000000\n", Rsp);
                E("movsd xmm1, [%s]\n", Rsp);
                E("add %s, 8\n", Rsp);
                E("xorpd xmm0, xmm1\n");
            } else {
                E("mov rax, 0x8000000000000000\n");
                E("movq xmm1, rax\n");
                E("xorpd xmm0, xmm1\n");
            }
        } else {
            E("neg %s\n", Rax);
        }
        return;

    /* ---- Logical not ---- */
    case ND_NOT:
        gen_expr(n->left);
        E("cmp %s, 0\n", Rax);
        E("sete al\n");
        E("movzx %s, al\n", Rax);
        return;

    /* ---- Bitwise not ---- */
    case ND_BITNOT:
        gen_expr(n->left);
        E("not %s\n", Rax);
        return;

    /* ---- Cast ---- */
    case ND_CAST: {
        gen_expr(n->left);
        Type *from = n->left->type;
        Type *to   = n->type;
        if (!from || !to) return;

        if (ty_is_float(from) && ty_is_integer(to)) {
            if (from->kind == TY_FLOAT) E("cvttss2si %s, xmm0\n", Rax);
            else                        E("cvttsd2si %s, xmm0\n", Rax);
            return;
        }
        if (ty_is_integer(from) && ty_is_float(to)) {
            if (to->kind == TY_FLOAT) E("cvtsi2ss xmm0, %s\n", Rax);
            else                      E("cvtsi2sd xmm0, %s\n", Rax);
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
        E("mov %s, %s\n", Rcx, Rax);
        load(n->left->type);
        if (n->kind == ND_PRE_INC) E("add %s, %d\n", Rax, delta);
        else                       E("sub %s, %d\n", Rax, delta);
        store(n->left->type);
        E("mov %s, %s\n", Rax, Rcx);
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
        E("mov %s, %s\n", Rcx, Rax);
        load(n->left->type);
        E("push %s\n", Rax);
        if (n->kind == ND_POST_INC) E("add %s, %d\n", Rax, delta);
        else                        E("sub %s, %d\n", Rax, delta);
        store(n->left->type);
        E("pop %s\n", Rcx);
        E("mov %s, %s\n", Rax, Rcx);
        return;
    }

    /* ---- Assignment ---- */
    case ND_ASSIGN: {
        gen_lvalue(n->left);
        E("push %s\n", Rax);
        gen_expr(n->right);
        E("pop %s\n", Rcx);
        store(n->left->type);
        E("mov %s, %s\n", Rax, Rcx);
        load(n->left->type);
        return;
    }

    /* ---- Compound assignment (+=, -=, ...) ---- */
    case ND_ASSIGN_OP: {
        gen_lvalue(n->left);
        E("push %s\n", Rax);
        load(n->left->type);
        E("push %s\n", Rax);
        gen_expr(n->right);
        E("pop %s\n", Rcx);
        E("xchg %s, %s\n", Rax, Rcx);

        switch (n->op) {
        case TK_PLUS_EQ:    E("add  %s, %s\n", Rax, Rcx); break;
        case TK_MINUS_EQ:   E("sub  %s, %s\n", Rax, Rcx); break;
        case TK_STAR_EQ:    E("imul %s, %s\n", Rax, Rcx); break;
        case TK_SLASH_EQ:
            if (m32) { E("cdq\n"); } else { E("cqo\n"); }
            E("idiv %s\n", Rcx); break;
        case TK_PERCENT_EQ:
            if (m32) { E("cdq\n"); } else { E("cqo\n"); }
            E("idiv %s\n", Rcx); E("mov %s, %s\n", Rax, Rdx); break;
        case TK_AMP_EQ:     E("and  %s, %s\n", Rax, Rcx); break;
        case TK_PIPE_EQ:    E("or   %s, %s\n", Rax, Rcx); break;
        case TK_CARET_EQ:   E("xor  %s, %s\n", Rax, Rcx); break;
        case TK_LSHIFT_EQ:  E("shl  %s, cl\n", Rax); break;
        case TK_RSHIFT_EQ:
            if (n->left->type && ty_is_signed(n->left->type))
                             E("sar  %s, cl\n", Rax);
            else             E("shr  %s, cl\n", Rax);
            break;
        default: break;
        }

        E("pop %s\n", Rcx);
        store(n->left->type);
        E("mov %s, %s\n", Rax, Rcx);
        load(n->left->type);
        return;
    }

    /* ---- Binary arithmetic / bitwise ---- */
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_SHL: case ND_SHR: case ND_BITAND: case ND_BITXOR: case ND_BITOR: {
        bool is_flt = n->type && ty_is_float(n->type);

        gen_expr(n->left);
        if (is_flt) { E("sub %s, 8\n", Rsp); E("movsd [%s], xmm0\n", Rsp); }
        else        { E("push %s\n", Rax); }

        gen_expr(n->right);

        if (is_flt) {
            E("movsd xmm1, [%s]\n", Rsp);
            E("add %s, 8\n", Rsp);
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

        E("pop %s\n", Rcx);

        switch (n->kind) {
        case ND_ADD:
            E("add %s, %s\n", Rax, Rcx);
            break;
        case ND_SUB:
            E("sub %s, %s\n", Rcx, Rax);
            E("mov %s, %s\n", Rax, Rcx);
            break;
        case ND_MUL:
            E("imul %s, %s\n", Rax, Rcx);
            break;
        case ND_DIV:
            E("xchg %s, %s\n", Rax, Rcx);
            if (m32) { E("cdq\n"); } else { E("cqo\n"); }
            E("idiv %s\n", Rcx);
            break;
        case ND_MOD:
            E("xchg %s, %s\n", Rax, Rcx);
            if (m32) { E("cdq\n"); } else { E("cqo\n"); }
            E("idiv %s\n", Rcx);
            E("mov %s, %s\n", Rax, Rdx);
            break;
        case ND_SHL:
            E("xchg %s, %s\n", Rax, Rcx);
            E("shl %s, cl\n", Rax);
            break;
        case ND_SHR:
            E("xchg %s, %s\n", Rax, Rcx);
            if (n->left->type && ty_is_signed(n->left->type))
                E("sar %s, cl\n", Rax);
            else
                E("shr %s, cl\n", Rax);
            break;
        case ND_BITAND: E("and %s, %s\n", Rax, Rcx); break;
        case ND_BITXOR: E("xor %s, %s\n", Rax, Rcx); break;
        case ND_BITOR:  E("or  %s, %s\n", Rax, Rcx); break;
        default: break;
        }
        return;
    }

    /* ---- Comparisons ---- */
    case ND_EQ: case ND_NEQ:
    case ND_LT: case ND_GT: case ND_LEQ: case ND_GEQ: {
        bool is_flt = n->left->type && ty_is_float(n->left->type);

        gen_expr(n->left);
        if (is_flt) { E("sub %s, 8\n", Rsp); E("movsd [%s], xmm0\n", Rsp); }
        else        { E("push %s\n", Rax); }

        gen_expr(n->right);

        if (is_flt) {
            E("movsd xmm1, [%s]\n", Rsp);
            E("add %s, 8\n", Rsp);
            E("ucomisd xmm1, xmm0\n");
        } else {
            E("pop %s\n", Rcx);
            E("cmp %s, %s\n", Rcx, Rax);
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
        E("movzx %s, al\n", Rax);
        return;
    }

    /* ---- Logical && ---- */
    case ND_LOGAND: {
        int lfalse = new_label(), lend = new_label();
        gen_expr(n->left);
        E("cmp %s, 0\n", Rax);
        E("je  .L%d\n", lfalse);
        gen_expr(n->right);
        E("cmp %s, 0\n", Rax);
        E("je  .L%d\n", lfalse);
        E("mov %s, 1\n", Rax);
        E("jmp .L%d\n", lend);
        emit_label(lfalse);
        E("mov %s, 0\n", Rax);
        emit_label(lend);
        return;
    }

    /* ---- Logical || ---- */
    case ND_LOGOR: {
        int ltrue = new_label(), lend = new_label();
        gen_expr(n->left);
        E("cmp %s, 0\n", Rax);
        E("jne .L%d\n", ltrue);
        gen_expr(n->right);
        E("cmp %s, 0\n", Rax);
        E("jne .L%d\n", ltrue);
        E("mov %s, 0\n", Rax);
        E("jmp .L%d\n", lend);
        emit_label(ltrue);
        E("mov %s, 1\n", Rax);
        emit_label(lend);
        return;
    }

    /* ---- Ternary ?: ---- */
    case ND_TERNARY: {
        int lelse = new_label(), lend = new_label();
        gen_expr(n->cond);
        E("cmp %s, 0\n", Rax);
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

        if (m32) {
            /* 32-bit cdecl: push all args right-to-left; caller cleans up */
            int total_bytes = 0;
            for (int i = n_args - 1; i >= 0; i--) {
                Node *arg = n->args[i];
                gen_expr(arg);
                if (arg->type && ty_is_float(arg->type)) {
                    if (arg->type->kind == TY_FLOAT) {
                        E("sub esp, 4\n");
                        E("movss [esp], xmm0\n");
                        total_bytes += 4;
                    } else {
                        E("sub esp, 8\n");
                        E("movsd [esp], xmm0\n");
                        total_bytes += 8;
                    }
                } else {
                    E("push eax\n");
                    total_bytes += 4;
                }
            }
            if (n->callee->kind == ND_IDENT && n->callee->sym)
                E("call %s\n", n->callee->sym->asm_name);
            else {
                gen_expr(n->callee);
                E("call eax\n");
            }
            if (total_bytes > 0) E("add esp, %d\n", total_bytes);
        } else {
            /* 64-bit SysV AMD64: first 6 int in regs, first 8 float in xmm */
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

            E("and rsp, -16\n");
            E("mov rax, %d\n", f_reg);

            if (n->callee->kind == ND_IDENT && n->callee->sym)
                E("call %s\n", n->callee->sym->asm_name);
            else {
                gen_expr(n->callee);
                E("call rax\n");
            }

            int extra = n_args > 6 ? (n_args - 6) : 0;
            if (extra > 0) E("add rsp, %d\n", extra * 8);
        }
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
        E("cmp %s, 0\n", Rax);
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
        E("cmp %s, 0\n", Rax);
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
        E("cmp %s, 0\n", Rax);
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
            E("cmp %s, 0\n", Rax);
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
        E("mov %s, %s\n", Rsw, Rax);    /* save switch value */

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
            E("cmp %s, %lld\n", Rsw, cases[i].val);
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
        E("mov %s, %s\n", Rsp, Rbp);
        E("pop %s\n", Rbp);
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
        if (m32) EL("\tdd .Lstr%d\n", n->str_id);
        else     EL("\tdq .Lstr%d\n", n->str_id);
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
        E("push %s\n", Rbp);
        E("mov  %s, %s\n", Rbp, Rsp);
        frame_size = n->offset;
        if (frame_size > 0) E("sub %s, %d\n", Rsp, frame_size);

        /* Copy arguments to their stack homes */
        if (m32) {
            /* 32-bit cdecl: args are at [ebp+8], [ebp+12], ... */
            int stack_off = 8;
            for (int i = 0; i < n->n_params; i++) {
                Node *pn = n->params[i];
                if (!pn->type) { stack_off += 4; continue; }
                if (ty_is_float(pn->type)) {
                    int fsz = (pn->type->kind == TY_FLOAT) ? 4 : 8;
                    if (pn->type->kind == TY_FLOAT) {
                        E("movss xmm0, [ebp+%d]\n", stack_off);
                        E("movss [ebp%+d], xmm0\n", pn->offset);
                    } else {
                        E("movsd xmm0, [ebp+%d]\n", stack_off);
                        E("movsd [ebp%+d], xmm0\n", pn->offset);
                    }
                    stack_off += fsz;
                } else {
                    E("mov eax, [ebp+%d]\n", stack_off);
                    E("mov [ebp%+d], eax\n", pn->offset);
                    stack_off += 4;
                }
            }
        } else {
            /* 64-bit SysV: first 6 int in regs, first 8 float in xmm */
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
        }

        /* Body */
        gen_stmt(n->body);

        /* Default return 0 (reachable when function has no explicit return) */
        E("mov %s, 0\n", Rax);
        E("mov %s, %s\n", Rsp, Rbp);
        E("pop %s\n", Rbp);
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
                if (m32) {
                    E("mov %s, %s\n", Rcx, Rbp);
                    if (n->sym->offset) E("add %s, %d\n", Rcx, n->sym->offset);
                } else {
                    E("lea %s, [%s%+d]\n", Rcx, Rbp, n->sym->offset);
                }
                store(n->type);
            } else if (n->type && n->type->size > 0) {
                /* Zero-initialise */
                if (m32) {
                    E("mov %s, %s\n", Rdi, Rbp);
                    if (n->sym->offset) E("add %s, %d\n", Rdi, n->sym->offset);
                } else {
                    E("lea %s, [%s%+d]\n", Rdi, Rbp, n->sym->offset);
                }
                E("mov %s, 0\n", Rax);
                int stride = m32 ? 4 : 8;
                for (int b = 0; b < n->type->size; b += stride) {
                    int rem = n->type->size - b;
                    if      (rem >= 8 && !m32) E("mov qword [%s+%d], %s\n", Rdi, b, Rax);
                    else if (rem >= 4) E("mov dword [%s+%d], eax\n", Rdi, b);
                    else if (rem >= 2) E("mov word  [%s+%d], ax\n",  Rdi, b);
                    else               E("mov byte  [%s+%d], al\n",  Rdi, b);
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
    init_mode();

    /* Collect extern symbols used by this translation unit */
    n_extern_syms = 0;
    for (int i = 0; i < prog->n_stmts; i++)
        collect_externs_node(prog->stmts[i]);

    /* File header */
    EL("; Generated by Neutron Compiler\n");
    if (m32) {
        EL("[bits 32]\n\n");
    } else {
        EL("[bits 64]\n");
        EL("[default rel]\n\n");
    }

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
