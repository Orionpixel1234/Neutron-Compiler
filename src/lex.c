/*
 * lex.c — Lexer (tokenizer) for Neutron
 *
 * Input:  preprocessed source text (with embedded "# line file" directives)
 * Output: singly-linked list of Token, terminated by TK_EOF
 *
 * Handles all C99 tokens:
 *   - Integer literals (decimal, hex 0x, octal 0, binary 0b, with suffixes)
 *   - Float literals (decimal, hex 0x, with e/E/p/P exponents, f/l suffixes)
 *   - Character literals (', with all escape sequences)
 *   - String literals (", adjacent strings are concatenated)
 *   - All operators and punctuators
 *   - All C99 keywords
 *   - Identifiers
 */

#include "../neutron.h"

/* ============================================================
 * Keyword table
 * ============================================================ */

static const struct { const char *word; TK kind; } kw_table[] = {
    { "auto",      TK_AUTO      },
    { "break",     TK_BREAK     },
    { "case",      TK_CASE      },
    { "char",      TK_CHAR      },
    { "const",     TK_CONST     },
    { "continue",  TK_CONTINUE  },
    { "default",   TK_DEFAULT   },
    { "do",        TK_DO        },
    { "double",    TK_DOUBLE    },
    { "else",      TK_ELSE      },
    { "enum",      TK_ENUM      },
    { "extern",    TK_EXTERN    },
    { "float",     TK_FLOAT     },
    { "for",       TK_FOR       },
    { "goto",      TK_GOTO      },
    { "if",        TK_IF        },
    { "inline",    TK_INLINE    },
    { "int",       TK_INT       },
    { "long",      TK_LONG      },
    { "register",  TK_REGISTER  },
    { "restrict",  TK_RESTRICT  },
    { "return",    TK_RETURN    },
    { "short",     TK_SHORT     },
    { "signed",    TK_SIGNED    },
    { "sizeof",    TK_SIZEOF    },
    { "static",    TK_STATIC    },
    { "struct",    TK_STRUCT    },
    { "switch",    TK_SWITCH    },
    { "typedef",   TK_TYPEDEF   },
    { "union",     TK_UNION     },
    { "unsigned",  TK_UNSIGNED  },
    { "void",      TK_VOID      },
    { "volatile",  TK_VOLATILE  },
    { "while",     TK_WHILE     },
    { "_Bool",     TK__BOOL     },
    { "_Alignof",  TK__ALIGNOF  },
    { "__alignof__", TK__ALIGNOF },
    { NULL, 0 }
};

/* ============================================================
 * Lexer state
 * ============================================================ */

typedef struct {
    const char *src;
    const char *p;      /* current position */
    const char *file;   /* current filename (from # line directives) */
    int         line;
    int         col;
} Lexer;

static void advance(Lexer *l) {
    if (*l->p == '\n') { l->line++; l->col = 1; }
    else               { l->col++; }
    l->p++;
}

static SrcLoc here(const Lexer *l) {
    return (SrcLoc){ l->file, l->line, l->col };
}

/* ============================================================
 * Token allocation helpers
 * ============================================================ */

static Token *new_token(TK kind, SrcLoc loc) {
    Token *t = xcalloc(1, sizeof *t);
    t->kind = kind;
    t->loc  = loc;
    return t;
}

/* ============================================================
 * Escape sequence decoder
 * ============================================================ */

/* Decode one escape sequence starting at *p (pointing after '\').
 * Advances *p past the sequence. */
static unsigned long long decode_escape(const char **p, SrcLoc loc) {
    char c = **p;
    (*p)++;
    switch (c) {
    case 'a':  return '\a';
    case 'b':  return '\b';
    case 'f':  return '\f';
    case 'n':  return '\n';
    case 'r':  return '\r';
    case 't':  return '\t';
    case 'v':  return '\v';
    case '\\': return '\\';
    case '\'': return '\'';
    case '"':  return '"';
    case '?':  return '?';
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
        /* Octal: up to 3 digits */
        unsigned long long v = c - '0';
        for (int i = 0; i < 2 && **p >= '0' && **p <= '7'; i++)
            v = v * 8 + (*(*p)++ - '0');
        return v;
    }
    case 'x': {
        /* Hex */
        unsigned long long v = 0;
        while (isxdigit((unsigned char)**p)) {
            char d = *(*p)++;
            v = v * 16 + (isdigit((unsigned char)d) ? d - '0' :
                          tolower((unsigned char)d) - 'a' + 10);
        }
        return v;
    }
    default:
        warn_at(loc, "unknown escape sequence '\\%c'", c);
        return c;
    }
}

/* ============================================================
 * Numeric literal lexer
 * ============================================================ */

static Token *lex_number(Lexer *l) {
    SrcLoc loc = here(l);
    const char *start = l->p;
    Token *t = new_token(TK_INT_LIT, loc);

    /* Determine base */
    int base = 10;
    if (l->p[0] == '0') {
        if (l->p[1] == 'x' || l->p[1] == 'X') { base = 16; l->p += 2; }
        else if (l->p[1] == 'b' || l->p[1] == 'B') { base = 2;  l->p += 2; }
        else if (isdigit((unsigned char)l->p[1]))   { base = 8;  l->p += 1; }
        else l->p++; /* bare 0 */
    }

    /* Read digits — also handle float detection */
    bool is_float = false;
    unsigned long long ival = 0;

    while (true) {
        char c = *l->p;
        int digit = -1;
        if      (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else if (c == '_') { l->p++; continue; } /* C23 digit separator */

        if (digit < 0 || digit >= base) break;
        ival = ival * base + digit;
        l->p++;
    }

    /* Float indicators */
    if (*l->p == '.' || *l->p == 'e' || *l->p == 'E' ||
        ((base == 16) && (*l->p == 'p' || *l->p == 'P'))) {
        is_float = true;
    }

    if (!is_float) {
        t->kind = TK_INT_LIT;
        t->ival = ival;
        /* Suffixes: u, l, ll, ul, ull, lu, llu */
        while (true) {
            char c = tolower((unsigned char)*l->p);
            if (c == 'u') { t->is_unsigned = true; l->p++; }
            else if (c == 'l') {
                l->p++;
                if (tolower((unsigned char)*l->p) == 'l') { t->is_llong = true; l->p++; }
                else t->is_long = true;
            } else break;
        }
        return t;
    }

    /* Float: re-parse using strtold for full precision */
    t->kind = TK_FLOAT_LIT;
    char *end;
    t->fval = strtold(start, &end);
    l->p = end;

    /* Suffix */
    if (*l->p == 'f' || *l->p == 'F') { t->is_float = true; l->p++; }
    else if (*l->p == 'l' || *l->p == 'L') { /* long double */ l->p++; }

    return t;
}

/* ============================================================
 * String literal lexer
 * ============================================================ */

static Token *lex_string(Lexer *l) {
    SrcLoc loc = here(l);
    l->p++;  /* skip opening " */

    char  buf[65536];
    int   len = 0;

    while (*l->p && *l->p != '"') {
        if (*l->p == '\n')
            error_at(loc, "unterminated string literal");
        unsigned long long c;
        if (*l->p == '\\') {
            l->p++;
            c = decode_escape(&l->p, loc);
        } else {
            c = (unsigned char)*l->p++;
        }
        if (len < (int)sizeof(buf) - 1)
            buf[len++] = (char)c;
    }
    if (*l->p == '"') l->p++;

    Token *t = new_token(TK_STR_LIT, loc);
    t->sval = xstrndup(buf, len);
    t->slen = len;
    return t;
}

/* ============================================================
 * Character literal lexer
 * ============================================================ */

static Token *lex_char(Lexer *l) {
    SrcLoc loc = here(l);
    l->p++;  /* skip opening ' */

    unsigned long long val = 0;
    if (*l->p == '\\') {
        l->p++;
        val = decode_escape(&l->p, loc);
    } else if (*l->p && *l->p != '\'') {
        val = (unsigned char)*l->p++;
    }

    if (*l->p == '\'') l->p++;
    else warn_at(loc, "unterminated character literal");

    Token *t = new_token(TK_CHAR_LIT, loc);
    t->ival = (long long)val;
    return t;
}

/* ============================================================
 * Identifier / keyword lexer
 * ============================================================ */

static Token *lex_ident(Lexer *l) {
    SrcLoc      loc   = here(l);
    const char *start = l->p;

    while (isalnum((unsigned char)*l->p) || *l->p == '_') l->p++;

    int   len  = (int)(l->p - start);
    char *text = xstrndup(start, len);

    /* Check keyword table */
    for (int i = 0; kw_table[i].word; i++) {
        if (strcmp(text, kw_table[i].word) == 0) {
            Token *t = new_token(kw_table[i].kind, loc);
            free(text);
            return t;
        }
    }

    Token *t = new_token(TK_IDENT, loc);
    t->sval = text;
    t->slen = len;
    return t;
}

/* ============================================================
 * Main lex loop
 * ============================================================ */

Token *lex(const char *filename, const char *src) {
    Lexer l;
    l.src  = src;
    l.p    = src;
    l.file = filename;
    l.line = 1;
    l.col  = 1;

    Token  head = {0};
    Token *tail = &head;

#define EMIT(t) do { tail->next = (t); tail = tail->next; } while(0)
#define SIMPLE(k) do { SrcLoc _loc = here(&l); l.p++; \
                        EMIT(new_token((k), _loc)); } while(0)

    while (*l.p) {
        /* Handle GCC/preprocessor line markers:  # <line> "<file>" */
        if (l.p[0] == '#' && (l.p == l.src || l.p[-1] == '\n')) {
            l.p++;
            /* skip spaces */
            while (*l.p == ' ' || *l.p == '\t') l.p++;
            if (isdigit((unsigned char)*l.p)) {
                l.line = (int)strtol(l.p, (char**)&l.p, 10) - 1;
                /* optional filename */
                while (*l.p == ' ' || *l.p == '\t') l.p++;
                if (*l.p == '"') {
                    l.p++;
                    const char *fs = l.p;
                    while (*l.p && *l.p != '"') l.p++;
                    l.file = xstrndup(fs, l.p - fs);
                    if (*l.p == '"') l.p++;
                }
            }
            /* skip to end of line */
            while (*l.p && *l.p != '\n') l.p++;
            continue;
        }

        /* Whitespace */
        if (isspace((unsigned char)*l.p)) {
            if (*l.p == '\n') { l.line++; l.col = 1; }
            else l.col++;
            l.p++;
            continue;
        }

        /* Line comment */
        if (l.p[0] == '/' && l.p[1] == '/') {
            l.p += 2;
            while (*l.p && *l.p != '\n') l.p++;
            continue;
        }

        /* Block comment */
        if (l.p[0] == '/' && l.p[1] == '*') {
            l.p += 2;
            while (*l.p && !(l.p[0] == '*' && l.p[1] == '/')) {
                if (*l.p == '\n') { l.line++; l.col = 1; }
                l.p++;
            }
            if (*l.p) l.p += 2;
            continue;
        }

        /* Numeric literal */
        if (isdigit((unsigned char)*l.p) ||
            (*l.p == '.' && isdigit((unsigned char)l.p[1]))) {
            EMIT(lex_number(&l));
            continue;
        }

        /* String literal */
        if (*l.p == '"') { EMIT(lex_string(&l)); continue; }

        /* Wide string L"..." — treat as regular for now */
        if (*l.p == 'L' && l.p[1] == '"') { l.p++; EMIT(lex_string(&l)); continue; }

        /* Character literal */
        if (*l.p == '\'') { EMIT(lex_char(&l)); continue; }

        /* Wide char L'...' */
        if (*l.p == 'L' && l.p[1] == '\'') { l.p++; EMIT(lex_char(&l)); continue; }

        /* Identifier or keyword */
        if (isalpha((unsigned char)*l.p) || *l.p == '_') {
            EMIT(lex_ident(&l));
            continue;
        }

        /* Operators and punctuators */
        SrcLoc loc = here(&l);
        char c0 = l.p[0], c1 = l.p[1], c2 = l.p[2];

        /* Three-character operators */
        if (c0 == '<' && c1 == '<' && c2 == '=') { l.p+=3; EMIT(new_token(TK_LSHIFT_EQ, loc)); continue; }
        if (c0 == '>' && c1 == '>' && c2 == '=') { l.p+=3; EMIT(new_token(TK_RSHIFT_EQ, loc)); continue; }
        if (c0 == '.' && c1 == '.' && c2 == '.') { l.p+=3; EMIT(new_token(TK_ELLIPSIS, loc));  continue; }

        /* Two-character operators */
        if (c0 == '+' && c1 == '+') { l.p+=2; EMIT(new_token(TK_INC, loc));       continue; }
        if (c0 == '-' && c1 == '-') { l.p+=2; EMIT(new_token(TK_DEC, loc));       continue; }
        if (c0 == '-' && c1 == '>') { l.p+=2; EMIT(new_token(TK_ARROW, loc));     continue; }
        if (c0 == '&' && c1 == '&') { l.p+=2; EMIT(new_token(TK_AND, loc));       continue; }
        if (c0 == '|' && c1 == '|') { l.p+=2; EMIT(new_token(TK_OR, loc));        continue; }
        if (c0 == '<' && c1 == '<') { l.p+=2; EMIT(new_token(TK_LSHIFT, loc));    continue; }
        if (c0 == '>' && c1 == '>') { l.p+=2; EMIT(new_token(TK_RSHIFT, loc));    continue; }
        if (c0 == '=' && c1 == '=') { l.p+=2; EMIT(new_token(TK_EQ, loc));        continue; }
        if (c0 == '!' && c1 == '=') { l.p+=2; EMIT(new_token(TK_NEQ, loc));       continue; }
        if (c0 == '<' && c1 == '=') { l.p+=2; EMIT(new_token(TK_LEQ, loc));       continue; }
        if (c0 == '>' && c1 == '=') { l.p+=2; EMIT(new_token(TK_GEQ, loc));       continue; }
        if (c0 == '+' && c1 == '=') { l.p+=2; EMIT(new_token(TK_PLUS_EQ, loc));   continue; }
        if (c0 == '-' && c1 == '=') { l.p+=2; EMIT(new_token(TK_MINUS_EQ, loc));  continue; }
        if (c0 == '*' && c1 == '=') { l.p+=2; EMIT(new_token(TK_STAR_EQ, loc));   continue; }
        if (c0 == '/' && c1 == '=') { l.p+=2; EMIT(new_token(TK_SLASH_EQ, loc));  continue; }
        if (c0 == '%' && c1 == '=') { l.p+=2; EMIT(new_token(TK_PERCENT_EQ, loc)); continue; }
        if (c0 == '&' && c1 == '=') { l.p+=2; EMIT(new_token(TK_AMP_EQ, loc));    continue; }
        if (c0 == '|' && c1 == '=') { l.p+=2; EMIT(new_token(TK_PIPE_EQ, loc));   continue; }
        if (c0 == '^' && c1 == '=') { l.p+=2; EMIT(new_token(TK_CARET_EQ, loc));  continue; }
        if (c0 == '#' && c1 == '#') { l.p+=2; continue; } /* ## token paste — skip */

        /* Single-character */
        switch (c0) {
        case '(': SIMPLE(TK_LPAREN);   break;
        case ')': SIMPLE(TK_RPAREN);   break;
        case '{': SIMPLE(TK_LBRACE);   break;
        case '}': SIMPLE(TK_RBRACE);   break;
        case '[': SIMPLE(TK_LBRACKET); break;
        case ']': SIMPLE(TK_RBRACKET); break;
        case ';': SIMPLE(TK_SEMI);     break;
        case ':': SIMPLE(TK_COLON);    break;
        case ',': SIMPLE(TK_COMMA);    break;
        case '.': SIMPLE(TK_DOT);      break;
        case '?': SIMPLE(TK_QUESTION); break;
        case '#': SIMPLE(TK_HASH);     break;
        case '+': SIMPLE(TK_PLUS);     break;
        case '-': SIMPLE(TK_MINUS);    break;
        case '*': SIMPLE(TK_STAR);     break;
        case '/': SIMPLE(TK_SLASH);    break;
        case '%': SIMPLE(TK_PERCENT);  break;
        case '&': SIMPLE(TK_AMP);      break;
        case '|': SIMPLE(TK_PIPE);     break;
        case '^': SIMPLE(TK_CARET);    break;
        case '~': SIMPLE(TK_TILDE);    break;
        case '!': SIMPLE(TK_BANG);     break;
        case '<': SIMPLE(TK_LT);       break;
        case '>': SIMPLE(TK_GT);       break;
        case '=': SIMPLE(TK_ASSIGN);   break;
        default:
            warn_at(loc, "unexpected character '%c' (0x%02x)", c0, (unsigned char)c0);
            l.p++;
            break;
        }
    }

    /* Concatenate adjacent string literals */
    for (Token *t = head.next; t && t->next; ) {
        if (t->kind == TK_STR_LIT && t->next->kind == TK_STR_LIT) {
            Token *n = t->next;
            int new_len = t->slen + n->slen;
            char *new_s = xmalloc(new_len + 1);
            memcpy(new_s, t->sval, t->slen);
            memcpy(new_s + t->slen, n->sval, n->slen);
            new_s[new_len] = '\0';
            free(t->sval);
            t->sval = new_s;
            t->slen = new_len;
            t->next = n->next;
            /* don't advance t — check again in case of three+ adjacent strings */
        } else {
            t = t->next;
        }
    }

    /* Append EOF */
    SrcLoc eof_loc = here(&l);
    EMIT(new_token(TK_EOF, eof_loc));

    return head.next;
}
