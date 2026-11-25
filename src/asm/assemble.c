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
    /* Variable value at the start of the current sizing pass. */
    int32_t snap_value;
    bool snap_defined;
} Symbol;

typedef enum {
    F_NONE,
    F_ACC,
    F_IMM,
    F_DIR,
    F_IND,
    F_IZX,
    F_IZY,
    F_IDL,
    F_IDY,
    F_SRY
} Form;

typedef struct {
    Form form;
    char index;
    char mod;
    char expr[ASM_LINE_MAX];
} Operand;

#define MACRO_ARGS 8
#define MACRO_DEPTH_MAX 15
#define INCLUDE_PATH_MAX 1024

typedef struct {
    char name[ASM_NAME_MAX + 1];
    char **lines;
    size_t nlines;
    size_t caplines;
    /* Run number of the most recent MAC line; a call is valid only after
     * that MAC line in the same run. */
    unsigned long def_run;
} Macro;

typedef struct {
    char from[ASM_NAME_MAX + 1];
    char to[ASM_NAME_MAX + 1];
} LabelMap;

/* Growable string used to build expanded macro lines. */
typedef struct {
    char *s;
    size_t len;
    size_t cap;
    bool oom;
} Buf;

/* A frame is the source file (frames[0]) or one expanded macro body. */
typedef struct {
    char **lines;
    size_t nlines;
    size_t next;
} Frame;

typedef struct {
    const char *path;
    /* Directory searched by USE when a file is not beside the source; may be NULL. */
    const char *macro_dir;
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
    uint8_t lst[4];
    int lst_n;
    bool acc16;
    bool idx16;
    bool dummy;
    int32_t dummy_pc;
    /* Assembling flag, and for each open DO/IF the flag it saved. */
    bool cond_on;
    bool *conds;
    size_t ncond;
    size_t capcond;
    bool in_cond;
    /* Open LUP: the loop body is replayed by moving frame next back to lup_next.
     * A LUP must end in the frame where it began (lup_frame). */
    bool lup_open;
    bool lup_skip;
    size_t lup_depth;
    size_t lup_frame;
    size_t lup_next;
    int32_t lup_count;
    int32_t lup_left;
    int ozunid;
    /* Macros persist across passes; run counts run_pass calls. */
    Macro *macros;
    size_t nmacros;
    size_t capmacros;
    unsigned long run;
    /* While a MAC body is being recorded, def is its macro and lines go there until EOM. */
    Macro *def;
    bool in_def;
    Frame *frames;
    size_t nframes;
    size_t capframes;
    /* Per-pass instruction count; indexes wide[] so each instruction has its own slot. */
    size_t wide_seq;
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

/* True when s is exactly the token MX (case-insensitive). */
static bool is_mx_token(const char *s)
{
    return toupper((unsigned char)s[0]) == 'M' && toupper((unsigned char)s[1]) == 'X'
        && !isalnum((unsigned char)s[2]) && s[2] != '_';
}

/* An IF operand c?]n, such as #=]1: its first character is compared with the
 * first character of argument n. The middle character is ignored. */
static bool char_test_operand(const char *op)
{
    return strlen(op) == 4 && op[2] == ']' && op[3] >= '0' && op[3] <= '0' + MACRO_ARGS;
}

static bool lookup_sym(void *ctx, const char *name, int32_t *value)
{
    Asm *a = ctx;
    char key[SYM_KEY_MAX];
    const Symbol *s;

    /* MX is a value only inside DO/IF operands, as in merlin32. */
    if (a->in_cond && is_mx_token(name)) {
        *value = (a->acc16 ? 0 : 2) | (a->idx16 ? 0 : 1);
        return true;
    }
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
    bool var = name[0] == ']';

    if (name[0] != ':' && !var) {
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
    /* Variables may be redefined at will; their layout effect is checked
     * per pass by vars_changed(). */
    if (var) {
        s->value = value;
        s->defined = defined;
        s->is_equ = true;
        return;
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

/* Variables are not layout-checked like labels, so a sizing pass that
 * leaves any variable with a different value than it started with needs
 * another pass. */
static void snapshot_vars(Asm *a)
{
    size_t i;

    for (i = 0; i < a->nsyms; i++) {
        if (a->syms[i].key[0] == ']') {
            a->syms[i].snap_value = a->syms[i].value;
            a->syms[i].snap_defined = a->syms[i].defined;
        }
    }
}

static bool vars_changed(const Asm *a)
{
    size_t i;

    for (i = 0; i < a->nsyms; i++) {
        const Symbol *s = &a->syms[i];

        if (s->key[0] == ']' && (s->value != s->snap_value || s->defined != s->snap_defined)) {
            return true;
        }
    }
    return false;
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
        if (a->pc > 0xFFFFFF) {
            diag(a, "program counter passes $FFFFFF");
        } else if (a->pass == 2 && !a->dummy && !bin_append(a->bin, bytes[i])) {
            diag(a, "out of memory");
        }
        if (a->pass == 2 && a->lst_n < 4) {
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
static bool hex_decode(Asm *a, const char *s, uint8_t *buf, size_t *n)
{
    size_t i = 0;
    size_t len = strlen(s);

    *n = 0;
    while (i < len) {
        if (i > 0 && s[i] == ',') {
            i++;
            if (i >= len || s[i] == ',') {
                diag(a, "bad comma in hex data");
                return false;
            }
            continue;
        }
        if (i + 1 >= len || !isxdigit((unsigned char)s[i]) || !isxdigit((unsigned char)s[i + 1])) {
            diag(a, "hex data needs digit pairs");
            return false;
        }
        buf[(*n)++] = (uint8_t)(hex_value((unsigned char)s[i]) * 16 + hex_value((unsigned char)s[i + 1]));
        i += 2;
    }
    return true;
}

static void emit_hex(Asm *a, const char *s)
{
    uint8_t buf[ASM_LINE_MAX];
    size_t n;

    if (hex_decode(a, s, buf, &n)) {
        emit(a, buf, (int)n);
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
    if (v < 0 || v > 0xFFFFFF) {
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

/*
 * DFB/DB (width 1), DW/DA (width 2, low byte first), and ADR/ADRL (width 3/4).
 * ADR/ADRL truncate to their width instead of range-checking, and a bare
 * ADR/ADRL emits nothing, both as merlin32 does.
 */
static void data_directive(Asm *a, const SrcLine *ln, int width)
{
    char item[ASM_LINE_MAX];
    const char *p = ln->operand;

    if (!ln->has_operand) {
        if (width == 1 || width == 2) {
            diag(a, "missing data operand");
        }
        return;
    }
    while (next_item(&p, item, sizeof item)) {
        const char *e = item;
        int32_t v = 0;
        bool defined = false;
        uint8_t bytes[4];

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
        } else if (width == 2) {
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
        } else {
            int32_t shifted = (bytes[0] == '>') ? (v >> 8) : v;
            int k;

            for (k = 0; k < width; k++) {
                bytes[k] = (uint8_t)((shifted >> (8 * k)) & 0xFF);
            }
            emit(a, bytes, width);
        }
    }
}

/* DUM sets the PC for label values without emitting bytes; DEND restores the PC. */
static void dum_directive(Asm *a, const SrcLine *ln)
{
    int32_t v = 0;
    bool defined = false;

    if (a->dummy) {
        diag(a, "DUM found before DEND");
        return;
    }
    if (!ln->has_operand) {
        diag(a, "DUM needs an address");
        return;
    }
    if (!eval_operand(a, ln->operand, &v, &defined)) {
        return;
    }
    if (v < 0 || v > 0xFFFFFF) {
        diag(a, "DUM address out of range");
        return;
    }
    a->dummy = true;
    a->dummy_pc = a->pc;
    a->pc = v;
}

static void dend_directive(Asm *a)
{
    if (a->dummy) {
        a->dummy = false;
        a->pc = a->dummy_pc;
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

enum text_kind { TEXT_ASC, TEXT_DCI, TEXT_STR, TEXT_INV, TEXT_FLS, TEXT_REV };

/* Shared by ASC, DCI, STR, INV, FLS, and REV. A '"' delimiter sets the high
 * bit of each character and a '\'' delimiter clears it. An optional hex
 * suffix follows the closing delimiter or a backslash. The bytes are built
 * in a buffer, then transformed to match merlin32:
 *   DCI  inverts the high bit of the last byte, including a hex suffix.
 *   STR  prefixes a length byte that counts the hex suffix too.
 *   INV  masks every byte with $3F, including a hex suffix.
 *   FLS  takes only $20-$5F characters and masks every byte with $7F.
 *   REV  reverses the string and rejects a hex suffix. */
static void text_directive(Asm *a, const SrcLine *ln, enum text_kind kind)
{
    uint8_t buf[ASM_LINE_MAX];
    const char *s = ln->operand;
    const char *end;
    const char *rest;
    size_t len;
    size_t n;
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

        if (kind == TEXT_FLS && (c < 0x20 || c > 0x5F)) {
            diag(a, "FLS string character out of range");
            return;
        }
        buf[i] = high ? (uint8_t)(c | 0x80) : (uint8_t)(c & 0x7F);
    }
    if (kind == TEXT_REV) {
        for (i = 0; i < len / 2; i++) {
            uint8_t t = buf[i];

            buf[i] = buf[len - 1 - i];
            buf[len - 1 - i] = t;
        }
    }
    n = len;
    if (*rest == ',' || *rest == '\\') {
        rest++;
    }
    if (*rest != '\0') {
        size_t hn;

        if (kind == TEXT_REV) {
            diag(a, "REV takes no hex suffix");
            return;
        }
        if (!hex_decode(a, rest, buf + n, &hn)) {
            return;
        }
        n += hn;
    }

    switch (kind) {
    case TEXT_DCI:
        if (n > 0) {
            buf[n - 1] ^= 0x80;
        }
        break;
    case TEXT_INV:
        for (i = 0; i < n; i++) {
            buf[i] &= 0x3F;
        }
        break;
    case TEXT_FLS:
        for (i = 0; i < n; i++) {
            buf[i] &= 0x7F;
        }
        break;
    case TEXT_STR:
        if (n > 255) {
            diag(a, "STR longer than 255 bytes");
            return;
        }
        {
            uint8_t count = (uint8_t)n;

            emit(a, &count, 1);
        }
        break;
    default:
        break;
    }
    emit(a, buf, (int)n);
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
        if (*p == '<' || *p == '>' || *p == '^') {
            o->mod = *p;
            p++;
        }
        return set_expr(a, o, p, strlen(p));
    }
    if (s[0] == '[') {
        if (n >= 5 && strcmp(s + n - 3, "],Y") == 0) {
            o->form = F_IDY;
            return set_expr(a, o, s + 1, n - 4);
        }
        if (s[n - 1] == ']') {
            o->form = F_IDL;
            return set_expr(a, o, s + 1, n - 2);
        }
        diag(a, "bad bracket operand");
        return false;
    }
    if (s[0] == '(') {
        if (n >= 5 && strcmp(s + n - 5, ",S),Y") == 0) {
            o->form = F_SRY;
            return set_expr(a, o, s + 1, n - 6);
        }
        if (n >= 4 && strcmp(s + n - 3, ",S)") == 0) {
            diag(a, "(sr,S) needs ,Y");
            return false;
        }
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
    if (s[0] == '<' || s[0] == '>') {
        o->mod = s[0];
        s++;
        n--;
    }
    if (n >= 2 && s[n - 2] == ',' && (toupper((unsigned char)s[n - 1]) == 'X' || toupper((unsigned char)s[n - 1]) == 'Y' || toupper((unsigned char)s[n - 1]) == 'S')) {
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

static bool put_long(Asm *a, int32_t v, uint8_t *out)
{
    if (v < 0 || v > 0xFFFFFF) {
        diag(a, "address out of range");
        return false;
    }
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
    out[2] = (uint8_t)((v >> 16) & 0xFF);
    return true;
}

/* REP and SEP are always 8-bit immediates; their operand updates the state
 * that later immediates use. */
static bool imm_is_wide(const Asm *a, const char *mnem)
{
    if (strcmp(mnem, "REP") == 0 || strcmp(mnem, "SEP") == 0 ||
        strcmp(mnem, "COP") == 0 || strcmp(mnem, "WDM") == 0) {
        return false;
    }
    if (strcmp(mnem, "LDX") == 0 || strcmp(mnem, "LDY") == 0 ||
        strcmp(mnem, "CPX") == 0 || strcmp(mnem, "CPY") == 0) {
        return a->idx16;
    }
    return a->acc16;
}

/* [dp], [dp],Y and (sr,S),Y. The [abs] form is JML/JMP only. */
static bool encode_bracket(Asm *a, const char *mnem, const Operand *o, uint8_t *out, int *size)
{
    int32_t v = 0;
    bool defined = false;
    AddrMode mode = (o->form == F_IDL) ? AM_IDL : (o->form == F_IDY) ? AM_IDY : AM_SRY;

    if (!eval_operand(a, o->expr, &v, &defined)) {
        return false;
    }
    if (o->form == F_IDL && !opcode_lookup(mnem, AM_IDL, &out[0])) {
        if (!opcode_lookup(mnem, AM_IAL, &out[0])) {
            diag(a, "%s does not support this addressing mode", mnem);
            return false;
        }
        if (!put_word(a, v, out + 1)) {
            return false;
        }
        *size = 3;
        return true;
    }
    if (!lookup_mode(a, mnem, mode, &out[0])) {
        return false;
    }
    if (v < 0 || v > 0xFF) {
        diag(a, "indirect operand must be zero page");
        return false;
    }
    out[1] = (uint8_t)v;
    *size = 2;
    return true;
}

/* MVN and MVP: operand is "src,dst" and the bytes are opcode, dst, src. */
static bool block_move(Asm *a, const char *mnem, const SrcLine *ln, uint8_t *out, int *size)
{
    char first[ASM_LINE_MAX];
    const char *comma = strchr(ln->operand, ',');
    size_t len;
    int32_t src = 0;
    int32_t dst = 0;
    bool defined = false;

    if (comma == NULL || comma == ln->operand || comma[1] == '\0') {
        diag(a, "%s needs two bank operands", mnem);
        return false;
    }
    len = (size_t)(comma - ln->operand);
    memcpy(first, ln->operand, len);
    first[len] = '\0';
    if (!eval_operand(a, first, &src, &defined) || !eval_operand(a, comma + 1, &dst, &defined)) {
        return false;
    }
    if (src < 0 || src > 0xFF || dst < 0 || dst > 0xFF) {
        diag(a, "%s bank must be 0..$FF", mnem);
        return false;
    }
    out[1] = (uint8_t)dst;
    out[2] = (uint8_t)src;
    *size = 3;
    return true;
}

static bool reserve_wide(Asm *a, size_t idx)
{
    size_t cap;
    uint8_t *grown;

    if (idx < a->capwide) {
        return true;
    }
    cap = a->capwide ? a->capwide : 256;
    while (cap <= idx) {
        cap *= 2;
    }
    grown = realloc(a->wide, cap);
    if (grown == NULL) {
        fprintf(stderr, "out of memory\n");
        return false;
    }
    memset(grown + a->capwide, 0, cap - a->capwide);
    a->wide = grown;
    a->capwide = cap;
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
    size_t idx = a->wide_seq++;

    *size = 0;
    if (!reserve_wide(a, idx)) {
        return false;
    }
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
        {
            bool wide = imm_is_wide(a, mnem);
            int shift = (o->mod == '>') ? 8 : (o->mod == '^') ? 16 : 0;
            int32_t part = (v >> shift) & (wide ? 0xFFFF : 0xFF);

            out[1] = (uint8_t)(part & 0xFF);
            out[2] = (uint8_t)((part >> 8) & 0xFF);
            *size = wide ? 3 : 2;
        }
        if (strcmp(mnem, "REP") == 0 || strcmp(mnem, "SEP") == 0) {
            bool set = mnem[0] == 'R';

            if (v & 0x20) {
                a->acc16 = set;
            }
            if (v & 0x10) {
                a->idx16 = set;
            }
        }
        return true;
    case F_IZX:
    case F_IZY:
        if (o->form == F_IZX && opcode_lookup(mnem, AM_IAX, &out[0])) {
            if (!eval_operand(a, o->expr, &v, &defined) || !put_word(a, v, out + 1)) {
                return false;
            }
            if (defined && v >= 0 && v <= 0xFF) {
                diag(a, "%s (addr,X) operand must be 16-bit", mnem);
                return false;
            }
            *size = 3;
            return true;
        }
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
        if (!eval_operand(a, o->expr, &v, &defined)) {
            return false;
        }
        if (opcode_lookup(mnem, AM_IND, &out[0])) {
            if (!put_word(a, v, out + 1)) {
                return false;
            }
            *size = 3;
            return true;
        }
        if (opcode_lookup(mnem, AM_IZP, &out[0])) {
            if (v < 0 || v > 0xFF) {
                diag(a, "indirect operand must be zero page");
                return false;
            }
            out[1] = (uint8_t)v;
            *size = 2;
            return true;
        }
        diag(a, "%s does not support this addressing mode", mnem);
        return false;
    case F_IDL:
    case F_IDY:
    case F_SRY:
        return encode_bracket(a, mnem, o, out, size);
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

    if (o->index == '\0' && opcode_lookup(mnem, AM_RL16, &out[0])) {
        if (o->mod != '\0') {
            diag(a, "branch operand cannot use a prefix");
            return false;
        }
        if (!eval_operand(a, o->expr, &v, &defined)) {
            return false;
        }
        if (defined) {
            int32_t off = v - (a->pc + 3);

            if (off < -32768 || off > 32767) {
                diag(a, "branch target out of range");
                return false;
            }
            out[1] = (uint8_t)(off & 0xFF);
            out[2] = (uint8_t)((off >> 8) & 0xFF);
        } else {
            out[1] = 0;
            out[2] = 0;
        }
        *size = 3;
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
    if (o->index == 'S') {
        if (!lookup_mode(a, mnem, AM_SR, &out[0])) {
            return false;
        }
        if (v < 0 || v > 0xFF) {
            diag(a, "stack-relative operand must be 0..$FF");
            return false;
        }
        out[1] = (uint8_t)v;
        *size = 2;
        return true;
    }
    if (o->mod == '>') {
        AddrMode lm = (o->index == 'X') ? AM_ALX : AM_ALG;

        if (o->index != '\0' && o->index != 'X') {
            diag(a, "%s has no long,%c mode", mnem, o->index);
            return false;
        }
        if (!lookup_mode(a, mnem, lm, &out[0]) || !put_long(a, v, out + 1)) {
            return false;
        }
        *size = 4;
        return true;
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
    if (o->index == '\0' && opcode_lookup(mnem, AM_ALG, &out[0])) {
        if (!put_long(a, v, out + 1)) {
            return false;
        }
        *size = 4;
        return true;
    }
    diag(a, "%s does not support this addressing mode", mnem);
    return false;
}

static void instruction(Asm *a, const char *mnem, bool forced, const SrcLine *ln)
{
    Operand o;
    uint8_t bytes[4] = { 0, 0, 0, 0 };
    int size = 0;

    if (opcode_lookup(mnem, AM_BLK, &bytes[0])) {
        if (block_move(a, mnem, ln, bytes, &size)) {
            emit(a, bytes, size);
        }
        return;
    }
    if (!parse_operand(a, ln->operand, &o)) {
        return;
    }
    if (encode_instruction(a, mnem, forced, &o, bytes, &size)) {
        emit(a, bytes, size);
    }
}

/* Bit 1 set = accumulator 8-bit, bit 0 set = index 8-bit (merlin32 behavior). */
static void mx_directive(Asm *a, const SrcLine *ln)
{
    int32_t v = 0;
    bool defined = false;

    if (!ln->has_operand) {
        diag(a, "MX needs an operand");
        return;
    }
    if (!eval_operand(a, ln->operand, &v, &defined)) {
        return;
    }
    if (v < 0 || v > 3) {
        diag(a, "MX value out of range");
        return;
    }
    a->acc16 = !(v & 2);
    a->idx16 = !(v & 1);
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
        text_directive(a, ln, TEXT_ASC);
    } else if (strcmp(name, "DCI") == 0) {
        text_directive(a, ln, TEXT_DCI);
    } else if (strcmp(name, "STR") == 0) {
        text_directive(a, ln, TEXT_STR);
    } else if (strcmp(name, "INV") == 0) {
        text_directive(a, ln, TEXT_INV);
    } else if (strcmp(name, "FLS") == 0) {
        text_directive(a, ln, TEXT_FLS);
    } else if (strcmp(name, "REV") == 0) {
        text_directive(a, ln, TEXT_REV);
    } else if (strcmp(name, "ADR") == 0) {
        data_directive(a, ln, 3);
    } else if (strcmp(name, "ADRL") == 0) {
        data_directive(a, ln, 4);
    } else if (strcmp(name, "DUM") == 0) {
        dum_directive(a, ln);
    } else if (strcmp(name, "DEND") == 0) {
        dend_directive(a);
    } else if (strcmp(name, "END") == 0) {
        a->ended = true;
    } else if (strcmp(name, "MX") == 0) {
        mx_directive(a, ln);
    } else if (strcmp(name, "LONGA") == 0 || strcmp(name, "LONGI") == 0) {
        /* merlin32 accepts these and ignores them; they do not change width. */
    } else {
        diag(a, "unknown opcode or directive: %s", name);
    }
}

static bool cond_active(const Asm *a)
{
    return a->cond_on;
}

/* Like merlin32, a nested DO inside a skipped block stays off, and ELSE
 * inverts the flag without consulting the enclosing block. */
static bool cond_push(Asm *a, bool value)
{
    bool *grown;

    if (a->ncond == a->capcond) {
        size_t cap = a->capcond ? a->capcond * 2 : 8;

        grown = realloc(a->conds, cap * sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        a->conds = grown;
        a->capcond = cap;
    }
    a->conds[a->ncond++] = a->cond_on;
    a->cond_on = a->cond_on && value;
    return true;
}

static bool is_conditional(const char *name)
{
    return strcmp(name, "DO") == 0 || strcmp(name, "IF") == 0
        || strcmp(name, "ELSE") == 0 || strcmp(name, "FIN") == 0;
}

static bool directive_known(const char *name)
{
    static const char *const names[] = {
        "ORG", "DS", "DFB", "DB", "DW", "DA", "HEX", "ASC", "DCI", "STR", "INV", "FLS", "REV",
        "ADR", "ADRL", "DUM", "DEND", "END", "MX", "LONGA", "LONGI", "EQU", "=", "KBD",
        "DO", "IF", "ELSE", "FIN",
    };
    size_t i;

    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(name, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Lines in a skipped block are not assembled, but merlin32 still rejects
 * mnemonics it does not recognize. */
static void skipped_op(Asm *a, const char *name)
{
    char base[4];

    snprintf(base, sizeof base, "%.3s", name);
    if (opcode_known(name) || directive_known(name)) {
        return;
    }
    if (strlen(name) == 4 && name[3] != 'D' && opcode_known(base)) {
        return;
    }
    diag(a, "unknown opcode or directive: %s", name);
}

static bool eval_cond_expr(Asm *a, const char *text, int32_t *value)
{
    bool defined = false;
    bool ok;

    a->in_cond = true;
    ok = eval_operand(a, text, value, &defined);
    a->in_cond = false;
    return ok;
}

/* A DO or IF operand is an expression, optionally compared with <, >, or =. */
static bool eval_condition(Asm *a, const char *text, int32_t *value)
{
    const char *rel = strpbrk(text, "<>=");
    char lhs[ASM_LINE_MAX];
    int32_t l = 0;
    int32_t r = 0;

    if (rel == NULL || rel == text) {
        return eval_cond_expr(a, text, value);
    }
    snprintf(lhs, sizeof lhs, "%.*s", (int)(rel - text), text);
    if (!eval_cond_expr(a, lhs, &l) || !eval_cond_expr(a, rel + 1, &r)) {
        return false;
    }
    switch (*rel) {
    case '<':
        *value = l < r;
        break;
    case '>':
        *value = l > r;
        break;
    default:
        *value = l == r;
        break;
    }
    return true;
}

static void conditional_op(Asm *a, const char *name, const SrcLine *ln)
{
    bool value = false;

    if (strcmp(name, "FIN") == 0) {
        if (a->ncond == 0) {
            diag(a, "FIN without DO or IF");
        } else {
            a->cond_on = a->conds[--a->ncond];
        }
        return;
    }
    if (strcmp(name, "ELSE") == 0) {
        if (a->ncond == 0) {
            diag(a, "ELSE without DO or IF");
        } else {
            a->cond_on = !a->cond_on;
        }
        return;
    }
    if (cond_active(a)) {
        if (!ln->has_operand) {
            diag(a, "%s needs an operand", name);
        } else if (strcmp(name, "IF") == 0 && char_test_operand(ln->operand)) {
            /* Outside a macro the argument is empty, so the test is false. */
            value = false;
        } else {
            int32_t v = 0;

            if (eval_condition(a, ln->operand, &v)) {
                value = v != 0;
            }
        }
    }
    if (!cond_push(a, value)) {
        fprintf(stderr, "out of memory\n");
    }
}

static Frame *cur_frame(Asm *a)
{
    return &a->frames[a->nframes - 1];
}

static void free_lines(char **lines, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        free(lines[i]);
    }
    free(lines);
}

static bool append_line(char ***lines, size_t *n, size_t *cap, const char *text)
{
    size_t len = strlen(text);
    char *copy;

    if (*n == *cap) {
        size_t newcap = *cap ? *cap * 2 : 64;
        char **grown = realloc(*lines, newcap * sizeof *grown);

        if (grown == NULL) {
            return false;
        }
        *lines = grown;
        *cap = newcap;
    }
    copy = malloc(len + 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, text, len + 1);
    (*lines)[(*n)++] = copy;
    return true;
}

static bool push_frame(Asm *a, char **lines, size_t nlines)
{
    Frame *f;

    if (a->nframes == a->capframes) {
        size_t cap = a->capframes ? a->capframes * 2 : 4;
        Frame *grown = realloc(a->frames, cap * sizeof *grown);

        if (grown == NULL) {
            fprintf(stderr, "out of memory\n");
            return false;
        }
        a->frames = grown;
        a->capframes = cap;
    }
    f = &a->frames[a->nframes++];
    f->lines = lines;
    f->nlines = nlines;
    f->next = 0;
    return true;
}

static void pop_frame(Asm *a)
{
    Frame *f = cur_frame(a);

    free_lines(f->lines, f->nlines);
    a->nframes--;
}

/* Compares an opcode with an uppercase word, ignoring case. */
static bool word_is(const char *s, const char *word)
{
    for (; *s != '\0' && *word != '\0'; s++, word++) {
        if (toupper((unsigned char)*s) != (unsigned char)*word) {
            return false;
        }
    }
    return *s == '\0' && *word == '\0';
}

/* Merlin32 ends a macro with EOM or with the alternate form <<<. */
static bool is_macro_end(const SrcLine *ln)
{
    return ln->has_opcode && (word_is(ln->opcode, "EOM") || strcmp(ln->opcode, "<<<") == 0);
}

/* Reads every line of f and closes it. A line longer than ASM_LINE_MAX is
 * reported and kept as an empty line. */
static bool read_lines(Asm *a, FILE *f, char ***lines, size_t *nlines)
{
    char buf[ASM_LINE_MAX + 2];
    char **out = NULL;
    size_t n = 0;
    size_t cap = 0;

    while (fgets(buf, sizeof buf, f) != NULL) {
        size_t len = strlen(buf);
        int ch;

        if (len > 0 && buf[len - 1] == '\n') {
            buf[--len] = '\0';
        } else if (!feof(f)) {
            a->line_no = (int)(n + 1);
            diag(a, "line too long");
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {
                ;
            }
            buf[0] = '\0';
            len = 0;
        }
        if (len > 0 && buf[len - 1] == '\r') {
            buf[--len] = '\0';
        }
        if (!append_line(&out, &n, &cap, buf)) {
            fprintf(stderr, "out of memory\n");
            free_lines(out, n);
            fclose(f);
            return false;
        }
    }
    fclose(f);
    *lines = out;
    *nlines = n;
    return true;
}

static bool load_source(Asm *a, char ***lines, size_t *nlines)
{
    FILE *f = fopen(a->path, "r");

    if (f == NULL) {
        fprintf(stderr, "%s: cannot open: %s\n", a->path, strerror(errno));
        return false;
    }
    return read_lines(a, f, lines, nlines);
}

/* Name PUT and USE open: three or more characters with no '.' get ".s";
 * anything else is used as written. */
static bool include_name(const char *operand, char *out, size_t cap)
{
    size_t n = strlen(operand);
    int len;

    if (n >= 3 && strchr(operand, '.') == NULL) {
        len = snprintf(out, cap, "%s.s", operand);
    } else {
        len = snprintf(out, cap, "%s", operand);
    }
    return len >= 0 && (size_t)len < cap;
}

/* Directory of path; "." when the path has no directory part. */
static bool dir_of(const char *path, char *dir, size_t cap)
{
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;

    if (slash == NULL) {
        return snprintf(dir, cap, ".") > 0;
    }
    if (n >= cap) {
        return false;
    }
    memcpy(dir, path, n);
    dir[n] = '\0';
    return true;
}

static FILE *open_in_dir(const char *dir, const char *name)
{
    char path[INCLUDE_PATH_MAX];
    int len;

    len = snprintf(path, sizeof path, "%s/%s", dir, name);
    if (len < 0 || (size_t)len >= sizeof path) {
        return NULL;
    }
    return fopen(path, "r");
}

/* Appends the lines of one PUT or USE file. Included files take no PUT or USE
 * lines of their own. A USE file found only in the macro directory keeps just
 * its MAC...EOM blocks, as merlin32 does. */
static bool include_file(Asm *a, const char *operand, bool use, char ***out, size_t *nout, size_t *cap)
{
    char name[INCLUDE_PATH_MAX];
    char dir[INCLUDE_PATH_MAX];
    char **lines = NULL;
    size_t nlines = 0;
    bool macro_only = false;
    bool in_mac = false;
    FILE *f = NULL;
    size_t i;

    if (operand[0] == '\0') {
        return true;
    }
    if (include_name(operand, name, sizeof name)) {
        if (dir_of(a->path, dir, sizeof dir)) {
            f = open_in_dir(dir, name);
        }
        if (f == NULL && use && a->macro_dir != NULL) {
            f = open_in_dir(a->macro_dir, name);
            macro_only = f != NULL;
        }
    }
    if (f == NULL) {
        diag(a, "Impossible to open Source file '%s'.", name);
        return true;
    }
    if (!read_lines(a, f, &lines, &nlines)) {
        return false;
    }
    for (i = 0; i < nlines; i++) {
        SrcLine ln;
        const char *err = NULL;
        bool has_op = parse_line(lines[i], &ln, &err) && ln.has_opcode;
        bool keep = true;
        bool is_eom = has_op && is_macro_end(&ln);

        if (has_op && (word_is(ln.opcode, "PUT") || word_is(ln.opcode, "USE"))) {
            keep = false;
        } else if (has_op && word_is(ln.opcode, "MAC")) {
            in_mac = true;
        } else if (is_eom) {
            in_mac = false;
        }
        if (macro_only && !in_mac && !is_eom) {
            keep = false;
        }
        if (keep && !append_line(out, nout, cap, lines[i])) {
            free_lines(lines, nlines);
            return false;
        }
    }
    free_lines(lines, nlines);
    return true;
}

/* Replaces each PUT and USE line of the source with the file it names, so
 * included lines take part in macros, LUP, and conditionals as if written
 * in place. */
static bool expand_includes(Asm *a, char ***lines, size_t *nlines)
{
    char **out = NULL;
    size_t n = 0;
    size_t cap = 0;
    size_t i;

    for (i = 0; i < *nlines; i++) {
        SrcLine ln;
        const char *err = NULL;
        bool ok;

        a->line_no = (int)(i + 1);
        if (parse_line((*lines)[i], &ln, &err) && ln.has_opcode &&
            (word_is(ln.opcode, "PUT") || word_is(ln.opcode, "USE"))) {
            ok = include_file(a, ln.has_operand ? ln.operand : "", word_is(ln.opcode, "USE"), &out, &n, &cap);
        } else {
            ok = append_line(&out, &n, &cap, (*lines)[i]);
        }
        if (!ok) {
            free_lines(out, n);
            free_lines(*lines, *nlines);
            *lines = NULL;
            *nlines = 0;
            return false;
        }
    }
    free_lines(*lines, *nlines);
    *lines = out;
    *nlines = n;
    return true;
}

static void buf_add(Buf *b, const char *s, size_t n)
{
    if (b->oom) {
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        char *grown;

        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        grown = realloc(b->s, cap);
        if (grown == NULL) {
            b->oom = true;
            return;
        }
        b->s = grown;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void buf_init(Buf *b)
{
    memset(b, 0, sizeof *b);
    buf_add(b, "", 0);
}

static bool name_start(int c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool name_char(int c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static Macro *lookup_macro(const Asm *a, const char *name)
{
    size_t i;

    for (i = 0; i < a->nmacros; i++) {
        if (strcmp(a->macros[i].name, name) == 0) {
            return &a->macros[i];
        }
    }
    return NULL;
}

/* A call is valid only after the MAC line for that macro in the current run,
 * so a forward call is an unknown opcode, as merlin32 reports it. */
static const Macro *find_macro(const Asm *a, const char *name)
{
    Macro *m = lookup_macro(a, name);

    return m != NULL && m->def_run == a->run ? m : NULL;
}

static void begin_macro(Asm *a, const char *name)
{
    Macro *m = lookup_macro(a, name);

    if (m == NULL) {
        Macro *grown;

        if (a->nmacros == a->capmacros) {
            size_t cap = a->capmacros ? a->capmacros * 2 : 8;

            grown = realloc(a->macros, cap * sizeof *grown);
            if (grown == NULL) {
                fprintf(stderr, "out of memory\n");
                return;
            }
            a->macros = grown;
            a->capmacros = cap;
        }
        m = &a->macros[a->nmacros++];
        memset(m, 0, sizeof *m);
        snprintf(m->name, sizeof m->name, "%s", name);
    } else if (m->def_run == a->run) {
        diag(a, "duplicate label");
    }
    free_lines(m->lines, m->nlines);
    m->lines = NULL;
    m->nlines = 0;
    m->caplines = 0;
    m->def_run = a->run;
    a->def = m;
    a->in_def = true;
}

/* Stores one line of a MAC body. The body ends at EOM in the opcode field. */
static void record_macro_line(Asm *a, const char *text)
{
    SrcLine ln;
    const char *err = NULL;

    if (parse_line(text, &ln, &err) && is_macro_end(&ln)) {
        a->in_def = false;
        a->def = NULL;
        return;
    }
    if (!append_line(&a->def->lines, &a->def->nlines, &a->def->caplines, text)) {
        fprintf(stderr, "out of memory\n");
    }
}

/* Splits the operand on ';' into arguments and returns how many there are.
 * Only the first MACRO_ARGS are kept. */
static size_t split_args(const char *operand, bool has_operand, char args[][ASM_LINE_MAX])
{
    size_t count = 0;
    const char *p = operand;

    if (!has_operand) {
        return 0;
    }
    for (;;) {
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);

        if (count < MACRO_ARGS) {
            memcpy(args[count], p, len);
            args[count][len] = '\0';
        }
        count++;
        if (semi == NULL) {
            break;
        }
        p = semi + 1;
    }
    return count;
}

/* Returns the label field of a macro body line, or false if it has none. */
static bool body_label(const char *line, char *label, size_t cap)
{
    size_t n = 0;

    if (line[0] == '\0' || line[0] == ' ' || line[0] == '\t' || line[0] == '*' || line[0] == ';') {
        return false;
    }
    while (line[n] != '\0' && line[n] != ' ' && line[n] != '\t') {
        if (n + 1 >= cap) {
            return false;
        }
        label[n] = line[n];
        n++;
    }
    label[n] = '\0';
    return true;
}

static const LabelMap *map_find(const LabelMap *map, size_t nmap, const char *name, size_t len)
{
    size_t i;

    for (i = 0; i < nmap; i++) {
        if (strlen(map[i].from) == len && strncmp(map[i].from, name, len) == 0) {
            return &map[i];
        }
    }
    return NULL;
}

/* Renames body labels to their per-expansion names. Quoted text and names
 * prefixed by $, %, :, or ] are left alone. */
static void rename_labels(const char *text, const LabelMap *map, size_t nmap, Buf *out)
{
    size_t i = 0;
    char quote = 0;

    while (text[i] != '\0') {
        char c = text[i];

        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
            buf_add(out, &text[i], 1);
            i++;
        } else if (c == '"' || c == '\'') {
            quote = c;
            buf_add(out, &text[i], 1);
            i++;
        } else if (name_start(c) && (i == 0 || !(name_char(text[i - 1]) || strchr(":]$%", text[i - 1])))) {
            size_t j = i;
            const LabelMap *hit;

            while (name_char(text[j])) {
                j++;
            }
            hit = map_find(map, nmap, text + i, j - i);
            if (hit != NULL) {
                buf_add(out, hit->to, strlen(hit->to));
            } else {
                buf_add(out, text + i, j - i);
            }
            i = j;
        } else {
            buf_add(out, &text[i], 1);
            i++;
        }
    }
}

/* ]0 is the argument count; ]1..]8 are the arguments. A missing argument is empty. */
static void substitute_args(const char *text, char args[][ASM_LINE_MAX], size_t nargs, Buf *out)
{
    size_t i = 0;

    while (text[i] != '\0') {
        if (text[i] == ']' && text[i + 1] >= '0' && text[i + 1] <= '0' + MACRO_ARGS) {
            size_t d = (size_t)(text[i + 1] - '0');

            if (d == 0) {
                char num[24];

                snprintf(num, sizeof num, "%zu", nargs);
                buf_add(out, num, strlen(num));
            } else if (d <= nargs) {
                buf_add(out, args[d - 1], strlen(args[d - 1]));
            }
            i += 2;
        } else {
            buf_add(out, &text[i], 1);
            i++;
        }
    }
}

/* An IF with a c?]n operand is decided while the arguments are known, so the
 * line becomes IF 1 or IF 0. Returns false for every other line. */
static bool resolve_char_test(const char *text, char args[][ASM_LINE_MAX], size_t nargs, Buf *out)
{
    SrcLine ln;
    const char *err = NULL;
    char line[ASM_NAME_MAX + 16];
    size_t n;
    char first = '\0';

    if (!parse_line(text, &ln, &err) || !ln.has_opcode || !word_is(ln.opcode, "IF") ||
        !ln.has_operand || !char_test_operand(ln.operand)) {
        return false;
    }
    n = (size_t)(ln.operand[3] - '0');
    if (n == 0) {
        first = (char)('0' + nargs);
    } else if (n <= nargs) {
        first = args[n - 1][0];
    }
    snprintf(line, sizeof line, "%s IF %d", ln.has_label ? ln.label : "",
             first != '\0' && first == ln.operand[0]);
    buf_add(out, line, strlen(line));
    return true;
}

/* True if an expanded line uses a ':' local name in its operand. Merlin
 * rejects these inside macro expansions, even though labels are accepted. */
static bool colon_operand(const char *text)
{
    SrcLine ln;
    const char *err = NULL;
    const char *p;
    char quote = 0;

    if (!parse_line(text, &ln, &err) || !ln.has_operand) {
        return false;
    }
    for (p = ln.operand; *p != '\0'; p++) {
        if (quote != 0) {
            if (*p == quote) {
                quote = 0;
            }
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == ':' && name_start((unsigned char)p[1]) &&
                   (p == ln.operand || !name_char((unsigned char)p[-1]))) {
            return true;
        }
    }
    return false;
}

/* Expands a macro call into a new frame. Each body label gets a fresh
 * ozunid_N name for this expansion, numbered from the shared counter. */
static void call_macro(Asm *a, const Macro *m, const char *operand, bool has_operand)
{
    char args[MACRO_ARGS][ASM_LINE_MAX];
    size_t nargs = split_args(operand, has_operand, args);
    size_t nbody = m->nlines;
    LabelMap *map;
    char **lines;
    size_t nmap = 0;
    size_t i;

    if (a->nframes > MACRO_DEPTH_MAX) {
        diag(a, "macro nesting too deep");
        return;
    }
    map = malloc((nbody ? nbody : 1) * sizeof *map);
    lines = calloc(nbody ? nbody : 1, sizeof *lines);
    if (map == NULL || lines == NULL) {
        free(map);
        free(lines);
        fprintf(stderr, "out of memory\n");
        return;
    }
    for (i = 0; i < nbody; i++) {
        char label[ASM_NAME_MAX + 1];

        if (body_label(m->lines[i], label, sizeof label) && name_start(label[0]) &&
            map_find(map, nmap, label, strlen(label)) == NULL) {
            snprintf(map[nmap].from, sizeof map[nmap].from, "%s", label);
            a->ozunid++;
            snprintf(map[nmap].to, sizeof map[nmap].to, "ozunid_%d", a->ozunid);
            nmap++;
        }
    }
    for (i = 0; i < nbody; i++) {
        Buf renamed;
        Buf expanded;

        buf_init(&renamed);
        buf_init(&expanded);
        rename_labels(m->lines[i], map, nmap, &renamed);
        if (!resolve_char_test(renamed.s, args, nargs, &expanded)) {
            substitute_args(renamed.s, args, nargs, &expanded);
        }
        lines[i] = expanded.s;
        if (renamed.oom || expanded.oom) {
            fprintf(stderr, "out of memory\n");
            free(renamed.s);
            free(map);
            free_lines(lines, nbody);
            return;
        }
        free(renamed.s);
        if (expanded.len > ASM_LINE_MAX) {
            diag(a, "line too long");
            expanded.s[0] = '\0';
        } else if (colon_operand(expanded.s)) {
            diag(a, "bad address mode: ':' local name in macro operand");
            expanded.s[0] = '\0';
        }
    }
    free(map);
    if (!push_frame(a, lines, nbody)) {
        free_lines(lines, nbody);
    }
}

/* NAME args calls a macro; PMC NAME calls it with no arguments. */
static void macro_line(Asm *a, const SrcLine *ln, bool pmc)
{
    const char *target = pmc ? ln->operand : ln->opcode;
    const Macro *m;

    if (pmc && !ln->has_operand) {
        diag(a, "PMC needs a macro name");
        return;
    }
    m = find_macro(a, target);
    if (m == NULL) {
        diag(a, "NOT MACRO");
        return;
    }
    if (ln->has_label) {
        define_label(a, ln->label, a->pc, true, false);
    }
    call_macro(a, m, pmc ? "" : ln->operand, !pmc && ln->has_operand);
}

/* A LUP count of zero, a negative count, or more than $8000 assembles
 * nothing, as in merlin32. */
static void lup_begin(Asm *a, const SrcLine *ln)
{
    int32_t count = 0;
    bool defined = false;

    if (!cond_active(a)) {
        return;
    }
    if (a->lup_open) {
        diag(a, "Impossible to locate end of Lup");
        return;
    }
    if (!ln->has_operand) {
        diag(a, "LUP needs a count");
        return;
    }
    if (!eval_operand(a, ln->operand, &count, &defined)) {
        return;
    }
    a->lup_open = true;
    a->lup_skip = count <= 0 || count > 0x8000;
    a->lup_depth = a->ncond;
    a->lup_frame = a->nframes;
    a->lup_next = cur_frame(a)->next;
    a->lup_count = count;
    a->lup_left = a->lup_skip ? 0 : count;
}

static void lup_end(Asm *a)
{
    if (!a->lup_open) {
        return;
    }
    if (a->ncond != a->lup_depth || a->lup_frame != a->nframes) {
        diag(a, "Impossible to locate end of Lup");
        a->lup_open = false;
        a->lup_skip = false;
        return;
    }
    if (!a->lup_skip && --a->lup_left > 0) {
        cur_frame(a)->next = a->lup_next;
        return;
    }
    a->lup_open = false;
    a->lup_skip = false;
}

/* Each @ label in a LUP becomes ozunid_N, numbered across the whole file. */
static void lup_label(Asm *a, SrcLine *ln)
{
    if (!a->lup_open || a->lup_skip || strchr(ln->label, '@') == NULL) {
        return;
    }
    a->ozunid++;
    snprintf(ln->label, sizeof ln->label, "ozunid_%d", a->ozunid);
    if (a->lup_count > 26 && a->lup_left == a->lup_count) {
        diag(a, "Impossible to update Label '@' (more than 26 counts in Lup)");
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

static void assemble_statement(Asm *a, const char *text);

static void assemble_line(Asm *a, const char *text)
{
    int32_t pc0 = a->pc;

    a->lst_n = 0;
    if (a->in_def) {
        record_macro_line(a, text);
    } else {
        assemble_statement(a, text);
    }

    if (a->pass == 2 && a->listing != NULL) {
        char hex[12];

        format_listing_bytes(a, hex, sizeof hex);
        fprintf(a->listing, "%5d  %04X  %-8s %s\n", a->line_no, (unsigned)pc0, hex, text);
    }
}

static void assemble_statement(Asm *a, const char *text)
{
    SrcLine ln;
    const char *err = NULL;
    char name[OPCODE_MAX];
    char base[4];
    size_t len;
    size_t i;
    bool forced = false;

    if (!parse_line(text, &ln, &err)) {
        diag(a, "%s", err);
        return;
    }
    if (ln.has_label && cond_active(a)) {
        lup_label(a, &ln);
    }
    if (!ln.has_opcode) {
        if (ln.has_label && cond_active(a)) {
            define_label(a, ln.label, a->pc, true, false);
        }
    } else {
        for (i = 0; ln.opcode[i] != '\0'; i++) {
            name[i] = (char)toupper((unsigned char)ln.opcode[i]);
        }
        name[i] = '\0';
        len = i;

        if (strcmp(name, "--^") == 0) {
            lup_end(a);
        } else if (a->lup_skip) {
            /* Body of a LUP that assembles zero times. */
        } else if (strcmp(name, "MAC") == 0) {
            if (!ln.has_label) {
                diag(a, "MAC needs a name");
            } else {
                begin_macro(a, ln.label);
            }
        } else if (strcmp(name, "LUP") == 0) {
            lup_begin(a, &ln);
        } else if (is_conditional(name)) {
            if (ln.has_label) {
                define_label(a, ln.label, a->pc, true, false);
            }
            conditional_op(a, name, &ln);
        } else if (!cond_active(a)) {
            if (strcmp(name, "PMC") != 0 && find_macro(a, ln.opcode) == NULL) {
                skipped_op(a, name);
            }
        } else if (strcmp(name, "PMC") == 0 || find_macro(a, ln.opcode) != NULL) {
            macro_line(a, &ln, strcmp(name, "PMC") == 0);
        } else if (strcmp(name, "EQU") == 0 || strcmp(name, "=") == 0) {
            int32_t v = 0;
            bool defined = false;

            if (!ln.has_label) {
                diag(a, "EQU needs a label");
            } else if (!ln.has_operand) {
                diag(a, "EQU needs a value");
            } else if (eval_operand(a, ln.operand, &v, &defined)) {
                define_label(a, ln.label, v, defined, true);
            }
        } else if (strcmp(name, "KBD") == 0) {
            /* merlin32 reads a key only interactively. With no key it sets
             * the label to the current PC, so the prompt operand is ignored. */
            if (!ln.has_label) {
                diag(a, "KBD needs a label");
            } else {
                define_label(a, ln.label, a->pc, true, false);
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
}

static bool run_pass(Asm *a, int pass)
{
    char **lines = NULL;
    size_t nlines = 0;

    a->pass = pass;
    a->line_no = 0;
    a->pc = 0;
    a->scope[0] = '\0';
    a->ended = false;
    a->acc16 = false;
    a->idx16 = false;
    a->dummy = false;
    a->ncond = 0;
    a->cond_on = true;
    a->in_cond = false;
    a->lup_open = false;
    a->lup_skip = false;
    a->ozunid = 0;
    a->wide_seq = 0;
    a->run++;

    while (a->nframes > 0) {
        pop_frame(a);
    }
    if (!load_source(a, &lines, &nlines)) {
        return false;
    }
    if (!expand_includes(a, &lines, &nlines)) {
        return false;
    }
    if (!push_frame(a, lines, nlines)) {
        free_lines(lines, nlines);
        return false;
    }

    while (!a->ended) {
        Frame *f = cur_frame(a);
        const char *text;

        if (f->next >= f->nlines) {
            if (a->nframes == 1) {
                break;
            }
            pop_frame(a);
            continue;
        }
        text = f->lines[f->next++];
        if (a->nframes == 1) {
            a->line_no = (int)f->next;
        }
        assemble_line(a, text);
    }
    while (a->nframes > 0) {
        pop_frame(a);
    }
    if (pass == 2 && a->ncond > 0) {
        diag(a, "missing FIN for DO or IF");
    }
    if (pass == 2 && a->lup_open) {
        diag(a, "Impossible to locate end of Lup");
    }
    return true;
}

static void free_macros(Asm *a)
{
    size_t i;

    while (a->nframes > 0) {
        pop_frame(a);
    }
    for (i = 0; i < a->nmacros; i++) {
        free_lines(a->macros[i].lines, a->macros[i].nlines);
    }
    free(a->macros);
}

bool assemble_source(const char *path, const char *macro_dir, FILE *listing, Binary *out)
{
    Asm a;
    bool ok;

    memset(&a, 0, sizeof a);
    out->bytes = NULL;
    out->len = 0;
    out->cap = 0;
    a.path = path;
    a.macro_dir = macro_dir;
    a.listing = listing;
    a.bin = out;

    ok = true;
    for (a.iter = 1; ok; a.iter++) {
        a.changed = false;
        snapshot_vars(&a);
        ok = run_pass(&a, 1);
        if (ok && vars_changed(&a)) {
            a.changed = true;
        }
        if (!ok || !a.changed) {
            break;
        }
        if (a.iter == MAX_LAYOUT_PASSES) {
            fprintf(stderr, "%s: layout does not settle after %d passes\n", path, MAX_LAYOUT_PASSES);
            ok = false;
        }
    }
    ok = ok && run_pass(&a, 2) && a.errors == 0;

    free_macros(&a);
    free(a.syms);
    free(a.wide);
    free(a.conds);
    free(a.frames);
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
