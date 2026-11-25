#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "asm.h"

typedef struct {
    const char *p;
    const ExprEnv *env;
    const char *err;
} Parser;

static bool set_err(Parser *ps, const char *msg)
{
    ps->err = msg;
    return false;
}

static int digit_value(int c, int base)
{
    int v;

    if (isdigit(c)) {
        v = c - '0';
    } else if (isxdigit(c)) {
        v = toupper(c) - 'A' + 10;
    } else {
        return -1;
    }
    return v < base ? v : -1;
}

static bool is_name_start(int c)
{
    return isalpha(c) || c == '_';
}

static bool is_name_char(int c)
{
    return isalnum(c) || c == '_';
}

static bool parse_number(Parser *ps, int base, ExprValue *v)
{
    const char *p = ps->p;
    int64_t n = 0;
    int d;

    if (digit_value((unsigned char)*p, base) < 0) {
        return set_err(ps, "expected digits");
    }
    while ((d = digit_value((unsigned char)*p, base)) >= 0) {
        n = n * base + d;
        if (n > 0xFFFFFFFF) {
            return set_err(ps, "number too large");
        }
        p++;
    }
    ps->p = p;
    v->value = (int32_t)n;
    v->defined = true;
    v->kind = REL_CONST;
    v->ext[0] = '\0';
    return true;
}

/* A quoted character: "A" has the high bit set, 'A' has it clear. */
static bool parse_char(Parser *ps, ExprValue *v)
{
    char quote = *ps->p++;
    unsigned char c = (unsigned char)*ps->p;

    if (c == '\0') {
        return set_err(ps, "unterminated character constant");
    }
    ps->p++;
    if (*ps->p == quote) {
        ps->p++;
    }
    v->value = (quote == '"') ? (c | 0x80) : (c & 0x7F);
    v->defined = true;
    v->kind = REL_CONST;
    v->ext[0] = '\0';
    return true;
}

/* A label, or a local label (leading ':') resolved by the environment. */
static bool parse_name(Parser *ps, ExprValue *v)
{
    char name[ASM_NAME_MAX + 1];
    const char *p = ps->p;
    size_t n = 0;

    if (*p == ':' || *p == ']') {
        name[n++] = *p++;
    }
    if (!is_name_start((unsigned char)*p)) {
        return set_err(ps, "expected a number, label, character, or '*'");
    }
    while (is_name_char((unsigned char)*p)) {
        if (n >= ASM_NAME_MAX) {
            return set_err(ps, "label too long");
        }
        name[n++] = *p++;
    }
    name[n] = '\0';
    ps->p = p;

    v->value = 0;
    v->kind = REL_CONST;
    v->ext[0] = '\0';
    v->defined = ps->env->lookup(ps->env->ctx, name, v);
    if (!v->defined) {
        v->value = 0;
        v->kind = REL_CONST;
        v->ext[0] = '\0';
    }
    return true;
}

static bool parse_primary(Parser *ps, ExprValue *v)
{
    bool negate = false;

    if (*ps->p == '-') {
        negate = true;
        ps->p++;
    }
    switch (*ps->p) {
    case '*':
        v->value = ps->env->pc;
        v->defined = true;
        v->kind = ps->env->rel ? REL_LOCAL : REL_CONST;
        v->ext[0] = '\0';
        ps->p++;
        break;
    case '$':
        ps->p++;
        if (!parse_number(ps, 16, v)) {
            return false;
        }
        break;
    case '%':
        ps->p++;
        if (!parse_number(ps, 2, v)) {
            return false;
        }
        break;
    case '"':
    case '\'':
        if (!parse_char(ps, v)) {
            return false;
        }
        break;
    default:
        if (isdigit((unsigned char)*ps->p)) {
            if (!parse_number(ps, 10, v)) {
                return false;
            }
        } else if (!parse_name(ps, v)) {
            return false;
        }
        break;
    }
    if (negate) {
        v->value = -v->value;
        if (v->kind != REL_CONST) {
            v->kind = REL_BAD;
        }
    }
    return true;
}

/* Relocation kind of acc OP rhs. Only the sums and differences that the
 * object format can express keep a relocatable kind; the rest are BAD. */
static void combine_rel(char op, ExprValue *acc, const ExprValue *rhs)
{
    RelKind a = acc->kind;
    RelKind b = rhs->kind;
    RelKind r = REL_BAD;
    const char *from = NULL;

    if (a == REL_CONST && b == REL_CONST) {
        r = REL_CONST;
    } else if (a == REL_BAD || b == REL_BAD) {
        r = REL_BAD;
    } else if (op == '+') {
        if (b == REL_CONST) {
            r = a;
            from = acc->ext;
        } else if (a == REL_CONST) {
            r = b;
            from = rhs->ext;
        }
    } else if (op == '-') {
        if (b == REL_CONST) {
            r = a;
            from = acc->ext;
        } else if (a == REL_LOCAL && b == REL_LOCAL) {
            r = REL_CONST;
        } else if (a == REL_EXT && b == REL_EXT && strcmp(acc->ext, rhs->ext) == 0) {
            r = REL_CONST;
        }
    }
    if (r == REL_EXT && from != NULL && from != acc->ext) {
        snprintf(acc->ext, sizeof acc->ext, "%s", from);
    } else if (r != REL_EXT) {
        acc->ext[0] = '\0';
    }
    acc->kind = r;
}

static bool apply_op(Parser *ps, char op, ExprValue *acc, ExprValue rhs)
{
    int64_t a = acc->value;
    int64_t b = rhs.value;
    int64_t r = 0;
    bool defined = acc->defined && rhs.defined;

    switch (op) {
    case '+':
        r = a + b;
        break;
    case '-':
        r = a - b;
        break;
    case '*':
        r = a * b;
        break;
    case '/':
        if (defined && b == 0) {
            return set_err(ps, "division by zero");
        }
        r = (defined && b != 0) ? a / b : 0;
        break;
    case '!':
        r = a ^ b;
        break;
    case '.':
        r = a | b;
        break;
    case '&':
        r = a & b;
        break;
    default:
        return set_err(ps, "unknown operator");
    }
    combine_rel(op, acc, &rhs);
    acc->value = (int32_t)r;
    acc->defined = defined;
    return true;
}

bool expr_eval(const char *text, const ExprEnv *env, ExprValue *out, const char **err)
{
    Parser ps = { text, env, NULL };
    ExprValue acc = { 0, false, REL_CONST, "" };
    ExprValue rhs;
    bool ok = parse_primary(&ps, &acc);

    while (ok && *ps.p != '\0') {
        char op = *ps.p;

        if (strchr("+-*/!.&", op) == NULL) {
            ok = set_err(&ps, "unexpected character in expression");
        } else {
            ps.p++;
            ok = parse_primary(&ps, &rhs) && apply_op(&ps, op, &acc, rhs);
        }
    }
    if (!ok) {
        *err = ps.err ? ps.err : "bad expression";
        return false;
    }
    *out = acc;
    return true;
}
