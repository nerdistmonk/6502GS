#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "line.h"

#define SYM_KEY_MAX (2 * ASM_NAME_MAX + 2)
#define MAX_LAYOUT_PASSES 32

typedef struct {
    char key[SYM_KEY_MAX];
    int32_t value;
    bool defined;
    bool is_equ;
    int def_line;
    int seen;
} Symbol;

typedef enum {
    F_NONE,
    F_ACC,
    F_IMM,
    F_DIR,
    F_IND,
    F_IZX,
    F_IZY
} Form;

typedef struct {
    Form form;
    char index;
    char mod;
    char expr[ASM_LINE_MAX];
} Operand;

typedef struct {
    const char *path;
    FILE *listing;
    Binary *bin;
    Symbol *syms;
    size_t nsyms;
    size_t capsyms;
    uint8_t *wide;
    size_t capwide;
    char scope[ASM_NAME_MAX + 1];
    int pass;
    int iter;
    bool changed;
    int line_no;
    int32_t pc;
    int errors;
    bool ended;
    uint8_t lst[3];
    int lst_n;
} Asm;

static void diag(Asm *a, const char *fmt, ...)
{
    va_list ap;

    if (a->pass != 2) {
        return;
    }
    a->errors++;
    fprintf(stderr, "%s:%d: ", a->path, a->line_no);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Local labels (leading ':') are qualified by the last global label. */
static bool make_key(const Asm *a, const char *name, char *key, size_t cap)
{
    int n;

    if (name[0] == ':') {
        n = snprintf(key, cap, "%s%s", a->scope, name);
    } else {
        n = snprintf(key, cap, "%s", name);
    }
    return n >= 0 && (size_t)n < cap;
}

static Symbol *sym_find(const Asm *a, const char *key)
{
    size_t i;

    for (i = 0; i < a->nsyms; i++) {
        if (strcmp(a->syms[i].key, key) == 0) {
            return &a->syms[i];
        }
    }
    return NULL;
}

static Symbol *sym_add(Asm *a, const char *key)
{
    Symbol *s;

    if (a->nsyms == a->capsyms) {
        size_t cap = a->capsyms ? a->capsyms * 2 : 64;
        Symbol *grown = realloc(a->syms, cap * sizeof *grown);

        if (grown == NULL) {
            return NULL;
        }
        a->syms = grown;
        a->capsyms = cap;
    }
    s = &a->syms[a->nsyms++];
    memset(s, 0, sizeof *s);
    snprintf(s->key, sizeof s->key, "%s", key);
    return s;
}

static bool lookup_sym(void *ctx, const char *name, int32_t *value)
{
    Asm *a = ctx;
    char key[SYM_KEY_MAX];
    const Symbol *s;

    if (!make_key(a, name, key, sizeof key)) {
        return false;
    }
    s = sym_find(a, key);
    if (s == NULL || !s->defined) {
        return false;
    }
    /* While sizing, a code label is visible only once defined in this pass,
     * so forward references get the 3-byte form. EQU values carry over from
     * earlier passes, which matches merlin32's zero-page choice for them. */
    if (a->pass == 1 && !s->is_equ && s->seen != a->iter) {
        return false;
    }
    *value = s->value;
    return true;
}

/* Sizing passes repeat until no symbol or instruction size changes; the
 * final pass (pass 2) then emits with that stable layout. */
static void define_label(Asm *a, const char *name, int32_t value, bool defined, bool equ)
{
    char key[SYM_KEY_MAX];
    Symbol *s;

    if (name[0] != ':') {
        snprintf(a->scope, sizeof a->scope, "%s", name);
    }
    if (!make_key(a, name, key, sizeof key)) {
        diag(a, "label too long: %s", name);
        return;
    }
    s = sym_find(a, key);
    if (s == NULL) {
        s = sym_add(a, key);
        if (s == NULL) {
            diag(a, "out of memory");
            return;
        }
    }
    if (s->def_line == 0) {
        s->def_line = a->line_no;
    } else if (s->def_line != a->line_no) {
        diag(a, "duplicate label: %s", name);
        return;
    }
    if (a->pass == 2 && a->errors == 0 && s->defined && (!defined || s->value != value)) {
        diag(a, "phase error: value of %s changed between passes", name);
    }
    if (!s->defined != !defined || s->value != value) {
        a->changed = true;
    }
    s->value = value;
    s->defined = defined;
    s->is_equ = equ;
    if (!equ) {
        s->seen = a->iter;
    }
}

static bool eval(Asm *a, const char *text, int32_t *value, bool *defined)
{
    ExprEnv env;
    ExprValue ev;
    const char *err = NULL;

    env.lookup = lookup_sym;
    env.ctx = a;
    env.pc = a->pc;
    if (!expr_eval(text, &env, &ev, &err)) {
        diag(a, "%s", err);
        return false;
    }
    *value = ev.value;
    *defined = ev.defined;
    return true;
}

/* Pass 1 tolerates undefined symbols; pass 2 reports them. */
static bool eval_operand(Asm *a, const char *text, int32_t *value, bool *defined)
{
    if (!eval(a, text, value, defined)) {
        return false;
    }
    if (!*defined) {
        *value = 0;
        if (a->pass == 2) {
            diag(a, "undefined symbol in '%s'", text);
            return false;
        }
    }
    return true;
}

static bool bin_append(Binary *bin, uint8_t b)
{
    if (bin->len == bin->cap) {
        size_t cap = bin->cap ? bin->cap * 2 : 1024;
        uint8_t *grown = realloc(bin->bytes, cap);

        if (grown == NULL) {
            return false;
        }
        bin->bytes = grown;
        bin->cap = cap;
    }
    bin->bytes[bin->len++] = b;
    return true;
}

static void emit(Asm *a, const uint8_t *bytes, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (a->pc > 0xFFFF) {
            diag(a, "program counter passes $FFFF");
        } else if (a->pass == 2 && !bin_append(a->bin, bytes[i])) {
            diag(a, "out of memory");
        }
        if (a->pass == 2 && a->lst_n < 3) {
            a->lst[a->lst_n++] = bytes[i];
        }
        a->pc++;
    }
}

static void emit_fill(Asm *a, int32_t count, uint8_t fill)
{
    int32_t i;

    for (i = 0; i < count; i++) {
        emit(a, &fill, 1);
    }
}

static int hex_value(int c)
{
    return isdigit(c) ? c - '0' : toupper(c) - 'A' + 10;
}

/* Hex pairs, adjacent or comma-separated. Diagnoses malformed digits. */
static void emit_hex(Asm *a, const char *s)
{
    size_t i = 0;
    size_t len = strlen(s);

    while (i < len) {
        uint8_t b;

        if (i > 0 && s[i] == ',') {
            i++;
            if (i >= len || s[i] == ',') {
                diag(a, "bad comma in hex data");
                return;
            }
            continue;
        }
        if (i + 1 >= len || !isxdigit((unsigned char)s[i]) || !isxdigit((unsigned char)s[i + 1])) {
            diag(a, "hex data needs digit pairs");
            return;
        }
        b = (uint8_t)(hex_value((unsigned char)s[i]) * 16 + hex_value((unsigned char)s[i + 1]));
        emit(a, &b, 1);
        i += 2;
    }
}

/* Copies the next comma-separated item; commas inside quotes stay. */
static bool next_item(const char **p, char *buf, size_t cap)
{
    const char *s = *p;
    size_t n = 0;
    char quote = 0;

    if (*s == '\0') {
        return false;
    }
    while (*s != '\0' && (quote != 0 || *s != ',')) {
        if (quote != 0) {
            if (*s == quote) {
                quote = 0;
            }
        } else if (*s == '"' || *s == '\'') {
            quote = *s;
        }
        if (n + 1 >= cap) {
            buf[0] = '\0';
            return false;
        }
        buf[n++] = *s++;
    }
    buf[n] = '\0';
    if (*s == ',') {
        s++;
    }
    *p = s;
    return true;
}

static void org_directive(Asm *a, const SrcLine *ln)
{
    int32_t v = 0;
    bool defined = false;

    if (!ln->has_operand) {
        diag(a, "ORG needs an address");
        return;
    }
    if (!eval_operand(a, ln->operand, &v, &defined)) {
        return;
    }
    if (v < 0 || v > 0xFFFF) {
        diag(a, "ORG address out of range");
        return;
    }
    a->pc = v;
}

/* DS n, DS n,fill, DS, DS \ and DS \expr (page-align with optional fill). */
static void ds_directive(Asm *a, const SrcLine *ln)
{
    char item[ASM_LINE_MAX];
    const char *p = ln->operand;
    int32_t count = 0;
    int32_t fill = 0;
    bool defined = false;

    if (!ln->has_operand || ln->operand[0] == '\\') {
        if (ln->has_operand && ln->operand[1] != '\0') {
            if (!eval_operand(a, ln->operand + 1, &fill, &defined)) {
                return;
            }
        }
        emit_fill(a, (0x100 - (a->pc & 0xFF)) & 0xFF, (uint8_t)(fill & 0xFF));
        return;
    }
    if (!next_item(&p, item, sizeof item)) {
        diag(a, "DS needs a size");
        return;
    }
    if (!eval_operand(a, item, &count, &defined)) {
        return;
    }
    if (count < 0 || count > 0xFFFF) {
        diag(a, "DS size out of range");
        return;
    }
    if (next_item(&p, item, sizeof item)) {
        if (!eval_operand(a, item, &fill, &defined)) {
            return;
        }
        if (count == 0) {
            diag(a, "DS fill form needs a positive size");
            return;
        }
    }
    emit_fill(a, count, (uint8_t)(fill & 0xFF));
}

/* DFB/DB (width 1) and DW/DA (width 2, low byte first). */
static void data_directive(Asm *a, const SrcLine *ln, int width)
{
    char item[ASM_LINE_MAX];
    const char *p = ln->operand;

    if (!ln->has_operand) {
        diag(a, "missing data operand");
        return;
    }
    while (next_item(&p, item, sizeof item)) {
        const char *e = item;
        int32_t v = 0;
        bool defined = false;
        uint8_t bytes[2];

        if (*e == '#') {
            e++;
        }
        if (*e == '<' || *e == '>') {
            bytes[0] = (uint8_t)*e;
            e++;
        } else {
            bytes[0] = '\0';
        }
        if (*e == '\0') {
            diag(a, "missing data expression");
            return;
        }
        if (!eval_operand(a, e, &v, &defined)) {
            return;
        }
        if (width == 1) {
            uint8_t b = (bytes[0] == '>') ? (uint8_t)((v >> 8) & 0xFF) : (uint8_t)(v & 0xFF);

            emit(a, &b, 1);
        } else {
            if (bytes[0] == '>') {
                v = (v >> 8) & 0xFFFF;
            }
            if (v < -0x8000 || v > 0xFFFF) {
                diag(a, "word value out of range");
                return;
            }
            bytes[0] = (uint8_t)(v & 0xFF);
            bytes[1] = (uint8_t)((v >> 8) & 0xFF);
            emit(a, bytes, 2);
        }
    }
}

static void hex_directive(Asm *a, const SrcLine *ln)
{
    if (!ln->has_operand) {
        diag(a, "HEX needs data");
        return;
    }
    if (strlen(ln->operand) > 64) {
        diag(a, "HEX operand longer than 64 characters");
        return;
    }
    emit_hex(a, ln->operand);
}

/* ASC/DCI: a '"' delimiter sets the high bit, a '\'' delimiter clears it.
 * DCI inverts the high bit of the last character. An optional hex suffix
 * follows the closing delimiter or a backslash. */
static void text_directive(Asm *a, const SrcLine *ln, bool inverted_last)
{
    const char *s = ln->operand;
    const char *end;
    const char *rest;
    size_t len;
    size_t i;
    char delim;
    bool high;

    if (!ln->has_operand) {
        diag(a, "missing string");
        return;
    }
    delim = s[0];
    if (delim != '"' && delim != '\'') {
        diag(a, "string must use \" or ' delimiters");
        return;
    }
    end = strchr(s + 1, delim);
    if (end == NULL) {
        end = strchr(s + 1, '\\');
        if (end == NULL) {
            diag(a, "unterminated string");
            return;
        }
        rest = end;
    } else {
        rest = end + 1;
    }
    len = (size_t)(end - (s + 1));
    high = delim == '"';
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[1 + i];
        uint8_t b = high ? (uint8_t)(c | 0x80) : (uint8_t)(c & 0x7F);

        if (inverted_last && i == len - 1) {
            b ^= 0x80;
        }
        emit(a, &b, 1);
    }
    if (*rest == ',' || *rest == '\\') {
        rest++;
    }
    if (*rest != '\0') {
        emit_hex(a, rest);
    }
}

static bool set_expr(Asm *a, Operand *o, const char *src, size_t n)
{
    if (n == 0) {
        diag(a, "missing operand expression");
        return false;
    }
    if (n >= sizeof o->expr) {
        diag(a, "operand too long");
        return false;
    }
    memcpy(o->expr, src, n);
    o->expr[n] = '\0';
    return true;
}

/* Classifies the operand text into a form, index register, and expression. */
static bool parse_operand(Asm *a, const char *s, Operand *o)
{
    size_t n = strlen(s);
    const char *p;

    memset(o, 0, sizeof *o);
    if (n == 0) {
        o->form = F_NONE;
        return true;
    }
    if (strcmp(s, "A") == 0) {
        o->form = F_ACC;
        return true;
    }
    if (s[0] == '#') {
        o->form = F_IMM;
        p = s + 1;
        if (*p == '<' || *p == '>') {
            o->mod = *p;
            p++;
        }
        return set_expr(a, o, p, strlen(p));
    }
    if (s[0] == '(') {
        if (n >= 4 && strcmp(s + n - 3, ",X)") == 0) {
            o->form = F_IZX;
            return set_expr(a, o, s + 1, n - 4);
        }
        if (n >= 4 && strcmp(s + n - 3, "),Y") == 0) {
            o->form = F_IZY;
            return set_expr(a, o, s + 1, n - 4);
        }
        if (n >= 2 && s[n - 1] == ')') {
            o->form = F_IND;
            return set_expr(a, o, s + 1, n - 2);
        }
        diag(a, "bad indirect operand");
        return false;
    }
    o->form = F_DIR;
    if (s[0] == '<') {
        o->mod = '<';
        s++;
        n--;
    }
    if (n >= 2 && s[n - 2] == ',' && (toupper((unsigned char)s[n - 1]) == 'X' || toupper((unsigned char)s[n - 1]) == 'Y')) {
        o->index = (char)toupper((unsigned char)s[n - 1]);
        n -= 2;
    }
    return set_expr(a, o, s, n);
}

static bool lookup_mode(Asm *a, const char *mnem, AddrMode mode, uint8_t *op)
{
    if (opcode_lookup(mnem, mode, op)) {
        return true;
    }
    diag(a, "%s does not support this addressing mode", mnem);
    return false;
}

static bool put_word(Asm *a, int32_t v, uint8_t *out)
{
    if (v < 0 || v > 0xFFFF) {
        diag(a, "address out of range");
        return false;
    }
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
    return true;
}

/* Picks the opcode and operand bytes. Pass 1 records in wide[] whether the
 * operand needs the 3-byte form (undefined or not zero page); pass 2 uses
 * that record so instruction sizes match between passes. */
static bool encode_instruction(Asm *a, const char *mnem, bool forced, const Operand *o, uint8_t *out, int *size)
{
    int32_t v = 0;
    bool defined = false;
    AddrMode small;
    AddrMode wide;
    size_t idx = (size_t)(a->line_no - 1);

    *size = 0;
    switch (o->form) {
    case F_NONE:
        if (opcode_lookup(mnem, AM_IMP, &out[0]) || opcode_lookup(mnem, AM_ACC, &out[0])) {
            *size = 1;
            return true;
        }
        diag(a, "%s needs an operand", mnem);
        return false;
    case F_ACC:
        if (!lookup_mode(a, mnem, AM_ACC, &out[0])) {
            return false;
        }
        *size = 1;
        return true;
    case F_IMM:
        if (!lookup_mode(a, mnem, AM_IMM, &out[0]) || !eval_operand(a, o->expr, &v, &defined)) {
            return false;
        }
        out[1] = (o->mod == '>') ? (uint8_t)((v >> 8) & 0xFF) : (uint8_t)(v & 0xFF);
        *size = 2;
        return true;
    case F_IZX:
    case F_IZY:
        if (!lookup_mode(a, mnem, o->form == F_IZX ? AM_IZX : AM_IZY, &out[0]) ||
            !eval_operand(a, o->expr, &v, &defined)) {
            return false;
        }
        if (v < 0 || v > 0xFF) {
            diag(a, "indirect operand must be zero page");
            return false;
        }
        out[1] = (uint8_t)v;
        *size = 2;
        return true;
    case F_IND:
        if (!lookup_mode(a, mnem, AM_IND, &out[0]) || !eval_operand(a, o->expr, &v, &defined) ||
            !put_word(a, v, out + 1)) {
            return false;
        }
        *size = 3;
        return true;
    case F_DIR:
        break;
    }

    if (o->index == '\0' && opcode_lookup(mnem, AM_REL, &out[0])) {
        if (o->mod != '\0') {
            diag(a, "branch operand cannot use '<'");
            return false;
        }
        if (!eval_operand(a, o->expr, &v, &defined)) {
            return false;
        }
        if (defined) {
            int32_t off = v - (a->pc + 2);

            if (off < -128 || off > 127) {
                diag(a, "branch target out of range");
                return false;
            }
            out[1] = (uint8_t)(off & 0xFF);
        } else {
            out[1] = 0;
        }
        *size = 2;
        return true;
    }

    if (o->index == 'X') {
        small = AM_ZPX;
        wide = AM_ABX;
    } else if (o->index == 'Y') {
        small = AM_ZPY;
        wide = AM_ABY;
    } else {
        small = AM_ZP;
        wide = AM_ABS;
    }
    if (!eval_operand(a, o->expr, &v, &defined)) {
        return false;
    }
    if (o->mod == '<') {
        v &= 0xFF;
        if (opcode_lookup(mnem, small, &out[0])) {
            out[1] = (uint8_t)v;
            *size = 2;
            return true;
        }
        if (opcode_lookup(mnem, wide, &out[0])) {
            if (!put_word(a, v, out + 1)) {
                return false;
            }
            *size = 3;
            return true;
        }
        diag(a, "%s does not support this addressing mode", mnem);
        return false;
    }
    if (a->pass == 1) {
        uint8_t w = (!defined || v < 0 || v > 0xFF) ? 1 : 0;

        if (a->wide[idx] != w) {
            a->changed = true;
        }
        a->wide[idx] = w;
    }
    if (!a->wide[idx] && !forced && v >= 0 && v <= 0xFF && opcode_lookup(mnem, small, &out[0])) {
        out[1] = (uint8_t)v;
        *size = 2;
        return true;
    }
    if (opcode_lookup(mnem, wide, &out[0])) {
        if (!put_word(a, v, out + 1)) {
            return false;
        }
        *size = 3;
        return true;
    }
    if (opcode_lookup(mnem, small, &out[0])) {
        if (v < 0 || v > 0xFF) {
            diag(a, "operand out of zero-page range");
            return false;
        }
        out[1] = (uint8_t)v;
        *size = 2;
        return true;
    }
    diag(a, "%s does not support this addressing mode", mnem);
    return false;
}

static void instruction(Asm *a, const char *mnem, bool forced, const SrcLine *ln)
{
    Operand o;
    uint8_t bytes[3] = { 0, 0, 0 };
    int size = 0;

    if (!parse_operand(a, ln->operand, &o)) {
        return;
    }
    if (encode_instruction(a, mnem, forced, &o, bytes, &size)) {
        emit(a, bytes, size);
    }
}

static void pseudo_op(Asm *a, const char *name, const SrcLine *ln)
{
    if (strcmp(name, "ORG") == 0) {
        org_directive(a, ln);
    } else if (strcmp(name, "DS") == 0) {
        ds_directive(a, ln);
    } else if (strcmp(name, "DFB") == 0 || strcmp(name, "DB") == 0) {
        data_directive(a, ln, 1);
    } else if (strcmp(name, "DW") == 0 || strcmp(name, "DA") == 0) {
        data_directive(a, ln, 2);
    } else if (strcmp(name, "HEX") == 0) {
        hex_directive(a, ln);
    } else if (strcmp(name, "ASC") == 0) {
        text_directive(a, ln, false);
    } else if (strcmp(name, "DCI") == 0) {
        text_directive(a, ln, true);
    } else if (strcmp(name, "END") == 0) {
        a->ended = true;
    } else {
        diag(a, "unknown opcode or directive: %s", name);
    }
}

static void format_listing_bytes(const Asm *a, char *buf, size_t cap)
{
    size_t n = 0;
    int i;

    buf[0] = '\0';
    for (i = 0; i < a->lst_n; i++) {
        n += (size_t)snprintf(buf + n, cap - n, "%s%02X", i ? " " : "", a->lst[i]);
    }
}

static void assemble_line(Asm *a, const char *text)
{
    SrcLine ln;
    const char *err = NULL;
    char name[OPCODE_MAX];
    char base[4];
    size_t len;
    size_t i;
    bool forced = false;
    int32_t pc0 = a->pc;

    a->lst_n = 0;
    if (!parse_line(text, &ln, &err)) {
        diag(a, "%s", err);
        return;
    }
    if (!ln.has_opcode) {
        if (ln.has_label) {
            define_label(a, ln.label, a->pc, true, false);
        }
    } else {
        for (i = 0; ln.opcode[i] != '\0'; i++) {
            name[i] = (char)toupper((unsigned char)ln.opcode[i]);
        }
        name[i] = '\0';
        len = i;

        if (strcmp(name, "EQU") == 0 || strcmp(name, "=") == 0) {
            int32_t v = 0;
            bool defined = false;

            if (!ln.has_label) {
                diag(a, "EQU needs a label");
            } else if (!ln.has_operand) {
                diag(a, "EQU needs a value");
            } else if (eval_operand(a, ln.operand, &v, &defined)) {
                define_label(a, ln.label, v, defined, true);
            }
        } else {
            if (ln.has_label) {
                define_label(a, ln.label, a->pc, true, false);
            }
            snprintf(base, sizeof base, "%.3s", name);
            if (len == 4 && name[3] != 'D' && opcode_known(base)) {
                forced = true;
                name[3] = '\0';
            }
            if (opcode_known(name)) {
                instruction(a, name, forced, &ln);
            } else {
                pseudo_op(a, name, &ln);
            }
        }
    }

    if (a->pass == 2 && a->listing != NULL) {
        char hex[12];

        format_listing_bytes(a, hex, sizeof hex);
        fprintf(a->listing, "%5d  %04X  %-8s %s\n", a->line_no, (unsigned)pc0, hex, text);
    }
}

static bool run_pass(Asm *a, int pass)
{
    FILE *f = fopen(a->path, "r");
    char buf[ASM_LINE_MAX + 2];

    if (f == NULL) {
        fprintf(stderr, "%s: cannot open: %s\n", a->path, strerror(errno));
        return false;
    }
    a->pass = pass;
    a->line_no = 0;
    a->pc = 0;
    a->scope[0] = '\0';
    a->ended = false;

    while (!a->ended && fgets(buf, sizeof buf, f) != NULL) {
        size_t len = strlen(buf);
        int ch;

        a->line_no++;
        if (len > 0 && buf[len - 1] == '\n') {
            buf[--len] = '\0';
        } else if (!feof(f)) {
            diag(a, "line too long");
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {
                ;
            }
            continue;
        }
        if (len > 0 && buf[len - 1] == '\r') {
            buf[--len] = '\0';
        }
        if (pass == 1 && (size_t)a->line_no > a->capwide) {
            size_t cap = a->capwide ? a->capwide : 256;
            uint8_t *grown;

            while (cap < (size_t)a->line_no) {
                cap *= 2;
            }
            grown = realloc(a->wide, cap);
            if (grown == NULL) {
                fprintf(stderr, "out of memory\n");
                fclose(f);
                return false;
            }
            memset(grown + a->capwide, 0, cap - a->capwide);
            a->wide = grown;
            a->capwide = cap;
        }
        assemble_line(a, buf);
    }
    fclose(f);
    return true;
}

bool assemble_source(const char *path, FILE *listing, Binary *out)
{
    Asm a;
    bool ok;

    memset(&a, 0, sizeof a);
    out->bytes = NULL;
    out->len = 0;
    out->cap = 0;
    a.path = path;
    a.listing = listing;
    a.bin = out;

    ok = true;
    for (a.iter = 1; ok; a.iter++) {
        a.changed = false;
        ok = run_pass(&a, 1);
        if (!ok || !a.changed) {
            break;
        }
        if (a.iter == MAX_LAYOUT_PASSES) {
            fprintf(stderr, "%s: layout does not settle after %d passes\n", path, MAX_LAYOUT_PASSES);
            ok = false;
        }
    }
    ok = ok && run_pass(&a, 2) && a.errors == 0;

    free(a.syms);
    free(a.wide);
    if (!ok) {
        binary_free(out);
    }
    return ok;
}

void binary_free(Binary *bin)
{
    free(bin->bytes);
    bin->bytes = NULL;
    bin->len = 0;
    bin->cap = 0;
}
