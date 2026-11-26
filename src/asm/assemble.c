#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "line.h"
#include "listing.h"

#define SYM_KEY_MAX (2 * ASM_NAME_MAX + 2)
#define MAX_LAYOUT_PASSES 32
#define OZ_FIRST_BASE 1000000

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
    /* Symbol table fields: name as printed (a local ":x" shows as "_x"),
     * definition order for ties, and the EQU operand as written. */
    char shown[ASM_NAME_MAX + 2];
    size_t def_order;
    char *text;
    /* The EQU value as the listing shows it: text in braces, with EQU names
     * replaced by their own listing form. */
    char *disp;
    bool oz_label;
    /* run_pass generation in which a ] label was last defined. */
    int oz_def_gen;
    int oz_gen;
    char oz_name[24];
    /* EXT names: the EXT line, the first label line that redefines the name,
     * and whether any expression read the name. */
    bool ext;
    bool ext_used;
    int ext_line;
    int ext_label_line;
    /* Relocation kind of the value; for an EXT name, the name itself. */
    RelKind kind;
    char ext_of[ASM_NAME_MAX + 1];
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
    /* Set by a leading ! or |, which forces the absolute form. */
    bool force_abs;
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
    /* File of the most recent MAC line, named in the duplicate-macro warning. */
    char file[INCLUDE_PATH_MAX];
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

/* Where a line of a frame came from: its listing file index, its line number
 * in that file, and whether it is assembled without appearing in the listing
 * (lines of a macro-only USE file). */
typedef struct {
    int file;
    int fline;
    bool silent;
} LineOrigin;

/* Whether a statement is listed: not consumed at all, consumed but hidden, or shown. */
enum lst_vis { LV_SKIP, LV_HIDE, LV_COUNT, LV_SHOW };

/* A frame is the source file (frames[0]) or one expanded macro body. org[i]
 * describes lines[i]. */
typedef struct {
    char **lines;
    LineOrigin *org;
    size_t nlines;
    size_t next;
} Frame;

/* The stage at which merlin32 reports an error. Pass 2 keeps only the error with
 * the lowest rank, so the one message printed matches merlin32's first message. */
enum err_rank {
    RANK_OPEN,
    RANK_UNKNOWN,
    RANK_MX,
    RANK_FIN,
    RANK_DUP_LABEL,
    RANK_MODE,
    RANK_DATA_FORMAT,
    RANK_EVAL,
    RANK_ERR,
    RANK_DATA_BUILD,
    RANK_JUMP,
    RANK_SIZE,
    RANK_OTHER
};

/* Which directive or instruction the current statement is, for undefined-symbol messages. */
enum err_ctx { CTX_NONE, CTX_INSTR, CTX_ORG, CTX_DS, CTX_DATA, CTX_ERR };

/* A growable list of AsmName records. */
typedef struct {
    AsmName *items;
    size_t n;
    size_t cap;
} NameList;

typedef struct {
    const char *path;
    /* Directory searched by USE when a file is not beside the source; may be NULL. */
    const char *macro_dir;
    /* True when pass 2 records listing rows. */
    bool listing;
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
    /* pc at the start of each pass: the default origin, or the link origin for a module. */
    int32_t start_pc;
    /* Link values for EXT names, and whether warnings are suppressed. */
    const AsmName *globals;
    size_t nglobals;
    bool quiet;
    /* Count of EXT name reads. An operand that reads one is assembled in its wide form. */
    size_t ext_refs;
    /* REL mode: labels are relocatable and fixups are recorded. */
    bool rel;
    /* Set by ORG in REL mode: later labels and `*` are absolute. */
    bool rel_abs;
    /* OMF link: every module is REL from the start, even without a REL directive. */
    bool force_rel;
    /* Set for link modules. A REL module in a flat link keeps its start address. */
    bool linked;
    /* Kind of the most recent successful expression evaluation. */
    ExprValue last_eval;
    /* ENT records from pass 2, then the resolved exports and the used EXT names. */
    NameList ents;
    NameList exports;
    NameList imports;
    int errors;
    bool ended;
    /* Listing rows and their bytes, and the per-line state that builds a row. */
    Listing lst;
    LineOrigin cur_org;
    unsigned long lst_seq;
    int nfiles;
    time_t started;
    enum lst_vis lst_vis;
    ListType lst_type;
    SrcLine lst_ln;
    /* Set while a DUM data directive runs: its bytes are listed as zero. */
    bool lst_zero;
    /* Whether the MAC being recorded is listed; false when its MAC line sits in a skipped block. */
    bool def_listed;
    bool acc16;
    bool idx16;
    bool dummy;
    int32_t dummy_pc;
    /* pc at the most recent non-DUM ORG (or the start), for the 64 KB object-size check. */
    int32_t seg_base;
    /* Where a bare ORG resumes: set by the first non-DUM ORG, then advanced by each non-DUM object byte. */
    int32_t org_loc;
    bool org_seen;
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
    /* Phase A numbers follow source order. Phase B numbers (LUP @ labels and
     * macro labels in LUP bodies) follow after all phase A numbers of the run,
     * so ozbase holds the phase A total of the previous run. */
    int ozunid;
    int ozbase;
    int ozb;
    int ozgen;
    /* Macros persist across passes; run counts run_pass calls. */
    Macro *macros;
    size_t nmacros;
    size_t capmacros;
    unsigned long run;
    /* While MAC bodies are being recorded, open_macs lists their indices in macros.
     * Stacked MAC headers all stay open, and each body line goes to every one until EOM. */
    size_t *open_macs;
    size_t nopen;
    size_t capopen;
    Frame *frames;
    size_t nframes;
    size_t capframes;
    /* Per-pass instruction count; indexes wide[] so each instruction has its own slot. */
    size_t wide_seq;
    size_t ndefs;
    /* The first error recorded at the lowest rank, already in merlin32's text. */
    bool err_has;
    int err_rank;
    char err_msg[4 * ASM_LINE_MAX + 256];
    enum err_ctx ctx;
    /* The statement being assembled; NULL between statements. */
    const SrcLine *stmt;
    const char *stmt_text;
} Asm;

/* Keeps the lowest-ranked error; the first one seen wins a tie. Callers in
 * pass 1 must not reach here, except for a source file that cannot be opened. */
static void record_v(Asm *a, int rank, const char *fmt, va_list ap)
{
    a->errors++;
    if (a->err_has && a->err_rank <= rank) {
        return;
    }
    a->err_has = true;
    a->err_rank = rank;
    vsnprintf(a->err_msg, sizeof a->err_msg, fmt, ap);
}

static void record(Asm *a, int rank, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    record_v(a, rank, fmt, ap);
    va_end(ap);
}

static void report(Asm *a, int rank, const char *fmt, ...)
{
    va_list ap;

    if (a->pass != 2) {
        return;
    }
    va_start(ap, fmt);
    record_v(a, rank, fmt, ap);
    va_end(ap);
}

/* An error without a merlin32 template keeps our wording, in merlin32's location form. */
static void diag(Asm *a, const char *fmt, ...)
{
    char text[ASM_LINE_MAX + 256];
    va_list ap;

    if (a->pass != 2) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    report(a, RANK_OTHER, "%s (line %d, file '%s').", text, a->line_no, a->path);
}

/* Warnings go to stderr in pass 2 and do not fail the build. */
static void warn(Asm *a, const char *fmt, ...)
{
    char text[ASM_LINE_MAX + 256];
    va_list ap;

    if (a->pass != 2 || a->quiet) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    fprintf(stderr, "      => [Warning] %s (line %d, file '%s').\n", text, a->line_no, a->path);
}

static const char *stmt_opcode(const Asm *a)
{
    return a->stmt != NULL ? a->stmt->opcode : "";
}

static const char *stmt_operand(const Asm *a)
{
    return (a->stmt != NULL && a->stmt->has_operand) ? a->stmt->operand : "";
}

/* Drops a trailing ,X, ,Y, or ,S. */
static void strip_index(char *s)
{
    size_t n = strlen(s);

    if (n >= 2 && s[n - 2] == ',' && strchr("XYS", toupper((unsigned char)s[n - 1])) != NULL) {
        s[n - 2] = '\0';
    }
}

/* The operand as merlin32 shows it in an address-mode message. */
static void merlin_shown(const char *operand, char *out, size_t cap)
{
    char buf[ASM_LINE_MAX];
    size_t n;

    snprintf(buf, sizeof buf, "%s", operand);
    if (buf[0] == '#') {
        const char *comma = strchr(buf, ',');

        snprintf(out, cap, "%s", comma != NULL ? comma + 1 : buf);
        return;
    }
    strip_index(buf);
    n = strlen(buf);
    if (n >= 2 && ((buf[0] == '(' && buf[n - 1] == ')') || (buf[0] == '[' && buf[n - 1] == ']'))) {
        buf[n - 1] = '\0';
        snprintf(out, cap, "%s", buf + 1);
        strip_index(out);
        return;
    }
    snprintf(out, cap, "%s", buf);
}

/* The name merlin32 reports as the element that could not be placed. */
static void merlin_element(const char *shown, char *out, size_t cap)
{
    const char *p = shown;
    size_t n;

    if (*p == '#' || *p == '<' || *p == '>' || *p == '^') {
        p++;
    }
    n = strcspn(p, "+-*/& ");
    snprintf(out, cap, "%.*s", (int)n, p);
}

static void mode_error(Asm *a)
{
    report(a, RANK_MODE,
        "Impossible to decode address mode for instruction '%s  %s' (line %d, file '%s').",
        stmt_opcode(a), stmt_operand(a), a->line_no, a->path);
}

static void element_error(Asm *a)
{
    char shown[ASM_LINE_MAX];
    char elem[ASM_LINE_MAX];
    const char *opr = stmt_operand(a);

    merlin_shown(opr, shown, sizeof shown);
    merlin_element(shown, elem, sizeof elem);
    report(a, RANK_MODE,
        "Impossible to decode address mode for instruction '%s  %s' (line %d, file '%s') : Can't find format for element '%s' in Operand '%s'.",
        stmt_opcode(a), opr, a->line_no, a->path, elem, shown);
}

static void jump_error(Asm *a)
{
    report(a, RANK_JUMP,
        "Bad Jump for instruction '%s  %s' (line %d, file '%s') : Too long.",
        stmt_opcode(a), stmt_operand(a), a->line_no, a->path);
}

static void unknown_line(Asm *a)
{
    report(a, RANK_UNKNOWN, "Unknown line '%s' in source file '%s' (line %d).",
        a->stmt_text != NULL ? a->stmt_text : "", a->path, a->line_no);
}

static void hex_error(Asm *a)
{
    report(a, RANK_DATA_FORMAT,
        "Impossible to decode Data format for instruction '%s  %s' (line %d, file '%s') : Wrong Hex Format Data in Operand '%s'.",
        stmt_opcode(a), stmt_operand(a), a->line_no, a->path, stmt_operand(a));
}

/* Reports an undefined symbol in the form merlin32 uses for the statement's
 * directive or instruction. text is the expression that failed. */
static void undefined_error(Asm *a, const char *text)
{
    const char *opr = stmt_operand(a);
    size_t n = strlen(opr);

    switch (a->ctx) {
    case CTX_ORG:
        report(a, RANK_EVAL,
            "Error : Impossible to evaluate ORG Address : '%s' (line %d, file '%s') : '%s' can't be translated as a numeric expression.",
            text, a->line_no, a->path, text);
        break;
    case CTX_DS:
        report(a, RANK_DATA_FORMAT,
            "Impossible to decode Data format for instruction '%s  %s' (line %d, file '%s') : Wrong DS Format Data in Operand '%s' ('%s' can't be translated as a numeric expression).",
            stmt_opcode(a), opr, a->line_no, a->path, text, text);
        break;
    case CTX_DATA:
        report(a, RANK_DATA_BUILD,
            "Impossible to build Data line '%s  %s' (line %d, file '%s') : Impossible to evaluate DB Data '%s' ('%s' can't be translated as a %s expression).",
            stmt_opcode(a), opr, a->line_no, a->path, text, text, text[0] == '$' ? "hexadecimal" : "numeric");
        break;
    case CTX_ERR:
        report(a, RANK_ERR,
            "Impossible to evaluate ERR expression '%s' (line %d, file '%s') : '%s' can't be translated as a numeric expression",
            text, a->line_no, a->path, text);
        break;
    case CTX_INSTR:
        if (n >= 2 && !strchr(opr, ',') && ((opr[0] == '(' && opr[n - 1] == ')') || (opr[0] == '[' && opr[n - 1] == ']'))) {
            char inner[ASM_LINE_MAX];

            snprintf(inner, sizeof inner, "%.*s", (int)(n - 2), opr + 1);
            report(a, RANK_EVAL,
                "Impossible to evaluate Operand '%s' for instruction '%s  %s' (line %d, file '%s') : '%s' can't be translated as a numeric expression.",
                inner, stmt_opcode(a), opr, a->line_no, a->path, inner);
        } else {
            element_error(a);
        }
        break;
    default:
        diag(a, "undefined symbol in '%s'", text);
        break;
    }
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

/* True when the operand s contains the token MX. */
static bool mentions_mx(const char *s)
{
    const char *p;

    for (p = s; *p != '\0'; p++) {
        if ((p == s || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')) && is_mx_token(p)) {
            return true;
        }
    }
    return false;
}

/* An IF operand c?]n, such as #=]1: its first character is compared with the
 * first character of argument n. The middle character is ignored. */
static bool char_test_operand(const char *op)
{
    return strlen(op) == 4 && op[2] == ']' && op[3] >= '0' && op[3] <= '0' + MACRO_ARGS;
}

static bool lookup_sym(void *ctx, const char *name, ExprValue *value)
{
    Asm *a = ctx;
    char key[SYM_KEY_MAX];
    Symbol *s;

    /* MX is a value only inside DO/IF operands, as in merlin32. */
    if (a->in_cond && is_mx_token(name)) {
        value->value = (a->acc16 ? 0 : 2) | (a->idx16 ? 0 : 1);
        value->kind = REL_CONST;
        value->ext[0] = '\0';
        return true;
    }
    if (!make_key(a, name, key, sizeof key)) {
        return false;
    }
    s = sym_find(a, key);
    if (s == NULL || !s->defined) {
        return false;
    }
    /* merlin32 rejects a forward reference to a label-style variable. */
    if (s->oz_label && s->oz_def_gen != a->ozgen) {
        return false;
    }
    /* While sizing, a code label is visible only once defined in this pass,
     * so forward references get the 3-byte form. EQU values carry over from
     * earlier passes, which matches merlin32's zero-page choice for them. */
    if (a->pass == 1 && !s->is_equ && s->seen != a->iter) {
        return false;
    }
    if (s->ext) {
        s->ext_used = true;
        a->ext_refs++;
    }
    value->value = s->value;
    value->kind = s->kind;
    snprintf(value->ext, sizeof value->ext, "%s", s->ext_of);
    return true;
}

/* Sizing passes repeat until no symbol or instruction size changes; the
 * final pass (pass 2) then emits with that stable layout. */
static void define_label_rel(Asm *a, const char *name, int32_t value, bool defined, bool equ, const ExprValue *rel)
{
    char key[SYM_KEY_MAX];
    Symbol *s;
    bool var = name[0] == ']';
    ExprValue plain = { 0, false, a->rel && !a->rel_abs ? REL_LOCAL : REL_CONST, "" };
    const ExprValue *kv = rel != NULL ? rel : &plain;

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
        s->kind = kv->kind;
        snprintf(s->ext_of, sizeof s->ext_of, "%s", kv->ext);
        if (!equ) {
            s->oz_label = true;
            s->oz_def_gen = a->ozgen;
        }
        return;
    }
    if (s->ext) {
        /* A label after its EXT line keeps the external value. */
        if (a->line_no > s->ext_line) {
            if (s->ext_label_line == 0) {
                s->ext_label_line = a->line_no;
            }
            warn(a, "label '%s' is also declared EXT; the external value is used", name);
        }
        return;
    }
    if (s->def_line == 0) {
        s->def_line = a->line_no;
        s->def_order = ++a->ndefs;
        if (name[0] == ':') {
            snprintf(s->shown, sizeof s->shown, "_%s", name + 1);
        } else {
            snprintf(s->shown, sizeof s->shown, "%s", name);
        }
    } else if (s->def_line != a->line_no) {
        report(a, RANK_DUP_LABEL, "Found label name '%s' in both source files '%s' (line %d) and '%s' (line %d).",
            name, a->path, s->def_line, a->path, a->line_no);
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
    s->kind = kv->kind;
    snprintf(s->ext_of, sizeof s->ext_of, "%s", kv->ext);
    if (!equ) {
        s->seen = a->iter;
    }
}

static void define_label(Asm *a, const char *name, int32_t value, bool defined, bool equ)
{
    define_label_rel(a, name, value, defined, equ, NULL);
}

/* Keeps an EQU operand as written so the symbol table can print it. */
static void buf_add(Buf *b, const char *s, size_t n);
static void buf_init(Buf *b);
static bool name_start(int c);
static bool name_char(int c);

/* Builds the listing form of an EQU value: the text in braces, where each
 * name that is already an EQU is replaced by that EQU's listing form. Numbers
 * and quoted text are copied unchanged. Returns NULL when out of memory. */
static char *equ_display(const Asm *a, const char *text)
{
    Buf b;
    const char *p = text;

    buf_init(&b);
    buf_add(&b, "{", 1);
    while (*p != '\0') {
        if (name_start((unsigned char)*p)) {
            const char *start = p;
            char tok[ASM_LINE_MAX];
            char key[SYM_KEY_MAX];
            Symbol *s;

            while (name_char((unsigned char)*p)) {
                p++;
            }
            snprintf(tok, sizeof tok, "%.*s", (int)(p - start), start);
            s = make_key(a, tok, key, sizeof key) ? sym_find(a, key) : NULL;
            if (s != NULL && s->is_equ && s->disp != NULL) {
                buf_add(&b, s->disp, strlen(s->disp));
            } else {
                buf_add(&b, start, (size_t)(p - start));
            }
        } else if (*p == '$' || isdigit((unsigned char)*p)) {
            const char *start = p;

            p++;
            while (isalnum((unsigned char)*p)) {
                p++;
            }
            buf_add(&b, start, (size_t)(p - start));
        } else if (*p == '\'' || *p == '"') {
            const char *start = p;
            char q = *p++;

            while (*p != '\0' && *p != q) {
                p++;
            }
            if (*p == q) {
                p++;
            }
            buf_add(&b, start, (size_t)(p - start));
        } else {
            buf_add(&b, p, 1);
            p++;
        }
    }
    buf_add(&b, "}", 1);
    if (b.oom) {
        free(b.s);
        return NULL;
    }
    return b.s;
}

static void set_equ_text(Asm *a, const char *name, const char *text)
{
    char key[SYM_KEY_MAX];
    Symbol *s;
    size_t len = strlen(text);

    if (!make_key(a, name, key, sizeof key)) {
        return;
    }
    s = sym_find(a, key);
    if (s == NULL) {
        return;
    }
    free(s->text);
    s->text = malloc(len + 1);
    if (s->text == NULL) {
        diag(a, "out of memory");
        return;
    }
    memcpy(s->text, text, len + 1);
    free(s->disp);
    s->disp = equ_display(a, text);
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

static bool eval_full(Asm *a, const char *text, bool grouped, ExprValue *ev)
{
    ExprEnv env;
    const char *err = NULL;

    env.lookup = lookup_sym;
    env.ctx = a;
    env.pc = a->pc;
    env.rel = a->rel && !a->rel_abs;
    if (!(grouped ? expr_eval_grouped : expr_eval)(text, &env, ev, &err)) {
        diag(a, "%s", err);
        return false;
    }
    a->last_eval = *ev;
    return true;
}

static bool eval(Asm *a, const char *text, int32_t *value, bool *defined)
{
    ExprValue ev;

    if (!eval_full(a, text, false, &ev)) {
        return false;
    }
    *value = ev.value;
    *defined = ev.defined;
    return true;
}

/* Pass 1 tolerates undefined symbols; pass 2 reports them. */
static bool eval_operand_mode(Asm *a, const char *text, bool grouped, ExprValue *ev)
{
    if (!eval_full(a, text, grouped, ev)) {
        return false;
    }
    if (!ev->defined) {
        ev->value = 0;
        ev->kind = REL_CONST;
        ev->ext[0] = '\0';
        if (a->pass == 2) {
            undefined_error(a, text);
            return false;
        }
    }
    return true;
}

static bool eval_operand_full(Asm *a, const char *text, ExprValue *ev)
{
    return eval_operand_mode(a, text, false, ev);
}

static bool eval_operand(Asm *a, const char *text, int32_t *value, bool *defined)
{
    ExprValue ev;

    if (!eval_operand_full(a, text, &ev)) {
        return false;
    }
    *value = ev.value;
    *defined = ev.defined;
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
        /* merlin32 limits: a DUM address wraps past $FFFF, and a non-DUM run may not pass $10000 (16-bit). */
        if (a->dummy) {
            if ((a->pc & 0xFFFF) == 0xFFFF) {
                diag(a, "DUM Object code size > 64 KB");
            }
        } else if ((a->seg_base & 0xFFFF) + (a->pc - a->seg_base) == 0x10000) {
            diag(a, "Object code size > 64 KB");
        }
        if (a->pc > 0xFFFFFF) {
            diag(a, "program counter passes $FFFFFF");
        } else if (a->pass == 2 && !a->dummy && !bin_append(a->bin, bytes[i])) {
            diag(a, "out of memory");
        }
        if (a->pass == 2 && a->listing) {
            listing_put_byte(&a->lst, a->lst_zero ? 0 : bytes[i]);
        }
        a->pc++;
        if (!a->dummy) {
            a->org_loc++;
        }
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
                hex_error(a);
                return false;
            }
            continue;
        }
        if (i + 1 >= len || !isxdigit((unsigned char)s[i]) || !isxdigit((unsigned char)s[i + 1])) {
            hex_error(a);
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
    int32_t v = a->org_loc;
    bool defined = false;

    if (ln->has_operand) {
        if (!eval_operand(a, ln->operand, &v, &defined)) {
            return;
        }
        if (v < 0 || v > 0xFFFFFF) {
            diag(a, "ORG address out of range");
            return;
        }
        if (!a->org_seen && !a->dummy) {
            a->org_loc = v;
            a->org_seen = true;
        }
    } else if (a->dummy) {
        return;
    }
    a->pc = v;
    if (!a->dummy) {
        a->seg_base = v;
    }
    if (a->rel) {
        a->rel_abs = true;
    }
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
        const char *f = ln->has_operand ? ln->operand + 1 : "";

        if (*f == ',') {
            f++;
        }
        if (*f != '\0' && !eval_operand(a, f, &fill, &defined)) {
            return;
        }
        emit_fill(a, 0x100 - (a->pc & 0xFF), (uint8_t)(fill & 0xFF));
        return;
    }
    if (!next_item(&p, item, sizeof item)) {
        diag(a, "DS needs a size");
        return;
    }
    if (!eval_operand(a, item, &count, &defined)) {
        return;
    }
    if (count < 0) {
        report(a, RANK_DATA_FORMAT,
            "Impossible to decode Data format for instruction '%s  %s' (line %d, file '%s') : Wrong DS Value in Operand '%s' : Negative value (%ld).",
            stmt_opcode(a), stmt_operand(a), a->line_no, a->path, item, (long)count);
        return;
    }
    if (count > 0xFFFF) {
        report(a, RANK_SIZE, "Error : %sObject code size > 64 KB (line %d, file '%s').", a->dummy ? "DUM " : "", a->line_no, a->path);
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
/* Records a patch for a relocatable value at body offset `offset`. Only the
 * pass that emits bytes records fixups. Returns false after a diagnostic. */
static bool record_fixup(Asm *a, size_t offset, const ExprValue *ev, int size, int shift)
{
    Binary *bin = a->bin;
    Fixup *f;

    if (!a->rel || a->pass != 2 || a->dummy || ev->kind == REL_CONST) {
        return true;
    }
    if (ev->kind == REL_BAD) {
        diag(a, "expression cannot be relocated");
        return false;
    }
    if (bin->nfix == bin->capfix) {
        size_t cap = bin->capfix ? bin->capfix * 2 : 32;
        Fixup *grown = realloc(bin->fix, cap * sizeof *grown);

        if (grown == NULL) {
            diag(a, "out of memory");
            return false;
        }
        bin->fix = grown;
        bin->capfix = cap;
    }
    f = &bin->fix[bin->nfix++];
    f->offset = offset;
    f->value = ev->value;
    f->size = size;
    f->shift = shift;
    f->external = ev->kind == REL_EXT;
    snprintf(f->name, sizeof f->name, "%s", f->external ? ev->ext : "");
    return true;
}

static int reloc_shift(char mod)
{
    if (mod == '>') {
        return -8;
    }
    if (mod == '^') {
        return -16;
    }
    return 0;
}

static void data_directive(Asm *a, const SrcLine *ln, int width, bool big)
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
        uint8_t bytes[4];
        ExprValue ev;

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
        if (!eval_operand_full(a, e, &ev)) {
            return;
        }
        v = ev.value;
        if (width == 1) {
            uint8_t b = (bytes[0] == '>') ? (uint8_t)((v >> 8) & 0xFF) : (uint8_t)(v & 0xFF);

            if (!record_fixup(a, a->bin->len, &ev, 1, bytes[0] == '>' ? -8 : 0)) {
                return;
            }
            emit(a, &b, 1);
        } else if (width == 2) {
            int shift = (bytes[0] == '>') ? -8 : 0;

            if (bytes[0] == '>') {
                v = (v >> 8) & 0xFFFF;
            }
            if (v < -0x8000) {
                diag(a, "word value out of range");
                return;
            }
            if (v > 0xFFFF) {
                warn(a, "value $%X truncated to 16 bits", (unsigned)v);
                v &= 0xFFFF;
            }
            bytes[0] = (uint8_t)(v & 0xFF);
            bytes[1] = (uint8_t)((v >> 8) & 0xFF);
            if (big) {
                uint8_t t = bytes[0];

                bytes[0] = bytes[1];
                bytes[1] = t;
                if (!record_fixup(a, a->bin->len, &ev, 1, -8) || !record_fixup(a, a->bin->len + 1, &ev, 1, 0)) {
                    return;
                }
            } else if (!record_fixup(a, a->bin->len, &ev, 2, shift)) {
                return;
            }
            emit(a, bytes, 2);
        } else {
            int32_t shifted = (bytes[0] == '>') ? (v >> 8) : v;
            int k;

            if (width == 4 && ev.kind != REL_CONST && a->pass == 2 && !a->dummy && ((shifted >> 24) & 0xFF) != 0) {
                diag(a, "relocatable value does not fit in 3 bytes");
                return;
            }
            if (!record_fixup(a, a->bin->len, &ev, width == 4 ? 3 : width, bytes[0] == '>' ? -8 : 0)) {
                return;
            }
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

/* CHK emits the exclusive OR of every byte assembled so far. */
static void chk_directive(Asm *a)
{
    uint8_t sum = 0;
    size_t i;

    if (a->pass == 2 && !a->dummy && a->bin != NULL) {
        for (i = 0; i < a->bin->len; i++) {
            sum ^= a->bin->bytes[i];
        }
    }
    /* merlin32 lists CHK's object code as 00; the binary keeps the checksum. */
    a->lst_zero = true;
    emit(a, &sum, 1);
}

enum text_kind { TEXT_ASC, TEXT_DCI, TEXT_STR, TEXT_STRL, TEXT_INV, TEXT_FLS, TEXT_REV };

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
    case TEXT_STRL: {
        uint8_t count[2];

        count[0] = (uint8_t)(n & 0xFF);
        count[1] = (uint8_t)((n >> 8) & 0xFF);
        emit(a, count, 2);
        break;
    }
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

/* Indirect operands may start with '<' or '>'. merlin32 ignores the prefix
 * for the value, but rejects some prefixed forms; encode_* decides. */
static bool set_indirect_expr(Asm *a, Operand *o, const char *src, size_t n)
{
    if (n > 0 && (src[0] == '<' || src[0] == '>')) {
        o->mod = src[0];
        src++;
        n--;
    }
    return set_expr(a, o, src, n);
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
        while (*p == '#') {
            p++;
        }
        if (*p == '<' || *p == '>' || *p == '^') {
            o->mod = *p;
            p++;
        }
        return set_expr(a, o, p, strlen(p));
    }
    if (s[0] == '[') {
        if (n >= 5 && strcmp(s + n - 3, "],Y") == 0) {
            o->form = F_IDY;
            return set_indirect_expr(a, o, s + 1, n - 4);
        }
        if (s[n - 1] == ']') {
            o->form = F_IDL;
            return set_indirect_expr(a, o, s + 1, n - 2);
        }
        element_error(a);
        return false;
    }
    if (s[0] == '(') {
        if (n >= 5 && strcmp(s + n - 5, ",S),Y") == 0) {
            o->form = F_SRY;
            return set_indirect_expr(a, o, s + 1, n - 6);
        }
        if (n >= 4 && strcmp(s + n - 3, ",S)") == 0) {
            diag(a, "(sr,S) needs ,Y");
            return false;
        }
        if (n >= 4 && strcmp(s + n - 3, ",X)") == 0) {
            o->form = F_IZX;
            return set_indirect_expr(a, o, s + 1, n - 4);
        }
        if (n >= 4 && strcmp(s + n - 3, "),Y") == 0) {
            o->form = F_IZY;
            return set_indirect_expr(a, o, s + 1, n - 4);
        }
        if (n >= 2 && s[n - 1] == ')') {
            o->form = F_IND;
            return set_indirect_expr(a, o, s + 1, n - 2);
        }
        element_error(a);
        return false;
    }
    o->form = F_DIR;
    if (s[0] == '!' || s[0] == '|') {
        o->force_abs = true;
        s++;
        n--;
    }
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
    mode_error(a);
    return false;
}

/* Operands wider than their slot keep the low bits, as merlin32 does. */
static bool put_word_trunc(Asm *a, int32_t v, uint8_t *out)
{
    if (v < 0) {
        mode_error(a);
        return false;
    }
    if (v > 0xFFFF) {
        warn(a, "value $%X truncated to 16 bits", (unsigned)v);
        v &= 0xFFFF;
    }
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
    return true;
}

/* Absolute operands above $FFFFFF are errors. */
static bool put_word(Asm *a, int32_t v, uint8_t *out)
{
    if (v > 0xFFFFFF) {
        mode_error(a);
        return false;
    }
    return put_word_trunc(a, v, out);
}

static bool put_long(Asm *a, int32_t v, uint8_t *out)
{
    if (v < 0) {
        mode_error(a);
        return false;
    }
    if (v > 0xFFFFFF) {
        warn(a, "value $%X truncated to 24 bits", (unsigned)v);
        v &= 0xFFFFFF;
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

    if (o->form == F_SRY && o->mod == '>') {
        mode_error(a);
        return false;
    }
    if (!eval_operand(a, o->expr, &v, &defined)) {
        return false;
    }
    if (o->form == F_IDL && !opcode_lookup(mnem, AM_IDL, &out[0])) {
        if (!opcode_lookup(mnem, AM_IAL, &out[0]) || o->mod == '<') {
            mode_error(a);
            return false;
        }
        if (!put_word_trunc(a, v, out + 1)) {
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
    ExprValue sv;
    ExprValue dv;

    if (comma == NULL || comma == ln->operand || comma[1] == '\0') {
        diag(a, "%s needs two bank operands", mnem);
        return false;
    }
    len = (size_t)(comma - ln->operand);
    memcpy(first, ln->operand, len);
    first[len] = '\0';
    if (!eval_operand_full(a, first, &sv) || !eval_operand_full(a, comma + 1, &dv)) {
        return false;
    }
    if (a->rel && (sv.kind != REL_CONST || dv.kind != REL_CONST)) {
        diag(a, "%s bank cannot be relocatable", mnem);
        return false;
    }
    src = sv.value;
    dst = dv.value;
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
    bool ext_ref = false;
    size_t refs;
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
            if (o->mod != '\0') {
                mode_error(a);
                return false;
            }
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
        if (o->mod == '>') {
            mode_error(a);
            return false;
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
            if (!put_word_trunc(a, v, out + 1)) {
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
        mode_error(a);
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
        refs = a->ext_refs;
        if (!eval_operand(a, o->expr, &v, &defined)) {
            return false;
        }
        if (a->ext_refs != refs) {
            jump_error(a);
            return false;
        }
        if (defined) {
            int32_t off = v - (a->pc + 2);

            if (off < -128 || off > 127) {
                jump_error(a);
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
        refs = a->ext_refs;
        if (!eval_operand(a, o->expr, &v, &defined)) {
            return false;
        }
        if (a->ext_refs != refs) {
            jump_error(a);
            return false;
        }
        if (defined) {
            int32_t off = v - (a->pc + 3);

            if (off < -32768 || off > 32767) {
                jump_error(a);
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
    refs = a->ext_refs;
    if (!eval_operand(a, o->expr, &v, &defined)) {
        return false;
    }
    ext_ref = a->ext_refs != refs;
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
        mode_error(a);
        return false;
    }
    if (a->pass == 1) {
        uint8_t w = (!defined || ext_ref || v < 0 || v > 0xFF) ? 1 : 0;

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
    mode_error(a);
    return false;
}

static bool branch_operand(const char *mnem, const Operand *o)
{
    uint8_t tmp;

    return o->form == F_DIR && o->index == '\0'
        && (opcode_lookup(mnem, AM_REL, &tmp) || opcode_lookup(mnem, AM_RL16, &tmp));
}

static void instruction(Asm *a, const char *mnem, bool forced, const SrcLine *ln)
{
    Operand o;
    uint8_t bytes[4] = { 0, 0, 0, 0 };
    int size = 0;

    a->ctx = CTX_INSTR;
    if (opcode_lookup(mnem, AM_BLK, &bytes[0])) {
        if (block_move(a, mnem, ln, bytes, &size)) {
            emit(a, bytes, size);
        }
        return;
    }
    if (!parse_operand(a, ln->operand, &o)) {
        return;
    }
    a->last_eval.kind = REL_CONST;
    if (encode_instruction(a, mnem, forced || o.force_abs, &o, bytes, &size)) {
        if (a->rel && size > 1 && !branch_operand(mnem, &o)
            && !record_fixup(a, a->bin->len + 1, &a->last_eval, size - 1, reloc_shift(o.mod))) {
            return;
        }
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
        report(a, RANK_MX, "Bad value '%s' for MX directive '%s' (line %d from file '%s') : .",
            ln->operand, ln->operand, a->line_no, a->path);
        return;
    }
    a->acc16 = !(v & 2);
    a->idx16 = !(v & 1);
}

static void err_directive(Asm *a, const SrcLine *ln)
{
    int32_t v = 0;
    bool defined = false;

    if (a->pass != 2) {
        return;
    }
    if (!ln->has_operand) {
        diag(a, "ERR needs an expression");
    } else if (eval_operand(a, ln->operand, &v, &defined)) {
        if (!defined) {
            undefined_error(a, ln->operand);
        } else if (v != 0) {
            report(a, RANK_ERR, "The evaluation of ERR expression '%s' is '0x%X' (line %d, file '%s')",
                ln->operand, (unsigned)v, a->line_no, a->path);
        }
    }
}

/* SAV and DSK both name the output file. As in merlin32, the first name wins
 * and later ones are ignored; an empty name is ignored. */
static void name_directive(Asm *a, const SrcLine *ln)
{
    const char *s = ln->operand;
    size_t n;

    if (a->pass != 2 || !ln->has_operand) {
        return;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    n = strcspn(s, " \t");
    /* merlin32 drops a trailing .L (any case) here, but not in link files. */
    if (n > 2 && s[n - 2] == '.' && (s[n - 1] == 'L' || s[n - 1] == 'l')) {
        n -= 2;
    }
    if (n == 0 || a->bin->out_name[0] != '\0') {
        return;
    }
    if (n >= sizeof a->bin->out_name) {
        diag(a, "output file name is too long");
        return;
    }
    memcpy(a->bin->out_name, s, n);
    a->bin->out_name[n] = '\0';
}

/* REL marks the source as a relocatable module. */
static void rel_directive(Asm *a, const SrcLine *ln)
{
    (void)ln;
    a->rel = true;
    a->bin->rel = true;
}

/* TYP keeps the low byte of its expression. Like merlin32, a missing or
 * undefined expression sets the type to 00 without an error. */
static void typ_directive(Asm *a, const SrcLine *ln)
{
    int32_t v = 0;
    bool defined = false;

    if (a->pass != 2) {
        return;
    }
    a->bin->typed = true;
    if (!ln->has_operand) {
        a->bin->file_type = 0;
    } else if (eval(a, ln->operand, &v, &defined)) {
        a->bin->file_type = defined ? (uint8_t)v : 0;
    }
}

static enum err_ctx directive_ctx(const char *name)
{
    if (strcmp(name, "ORG") == 0) {
        return CTX_ORG;
    }
    if (strcmp(name, "DS") == 0) {
        return CTX_DS;
    }
    if (strcmp(name, "DFB") == 0 || strcmp(name, "DB") == 0 ||
        strcmp(name, "DW") == 0 || strcmp(name, "DA") == 0) {
        return CTX_DATA;
    }
    if (strcmp(name, "ERR") == 0) {
        return CTX_ERR;
    }
    return CTX_NONE;
}

/* Copies the next comma-separated name from an EXT or ENT operand. The
 * name must be non-empty and fit in cap; *pp moves past the comma. */
static bool next_name(const char **pp, char *out, size_t cap)
{
    const char *p = *pp;
    size_t n = 0;

    while (*p != '\0' && *p != ',') {
        if (n + 1 >= cap) {
            return false;
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    if (*p == ',') {
        p++;
    }
    *pp = p;
    return n > 0;
}

static bool name_list_add(NameList *l, const char *name, int32_t value, int line)
{
    AsmName *items;

    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;

        items = realloc(l->items, cap * sizeof *items);
        if (items == NULL) {
            return false;
        }
        l->items = items;
        l->cap = cap;
    }
    snprintf(l->items[l->n].name, sizeof l->items[l->n].name, "%s", name);
    l->items[l->n].value = value;
    l->items[l->n].line = line;
    l->n++;
    return true;
}

/* EXT names a label whose value comes from another module. A name already
 * defined here as a label becomes external from this line on. */
static void ext_directive(Asm *a, const SrcLine *ln)
{
    const char *p;
    char name[ASM_NAME_MAX + 1];

    if (!ln->has_operand) {
        diag(a, "EXT needs a label name");
        return;
    }
    p = ln->operand;
    while (*p != '\0') {
        Symbol *s;
        int32_t value = 0;
        size_t i;

        if (!next_name(&p, name, sizeof name)) {
            diag(a, "bad EXT operand '%s'", ln->operand);
            return;
        }
        /* Local labels cannot be external, so a ':' name is ignored. */
        if (name[0] == ':') {
            continue;
        }
        s = sym_find(a, name);
        if (s == NULL) {
            s = sym_add(a, name);
            if (s == NULL) {
                diag(a, "out of memory");
                return;
            }
        }
        if (s->ext) {
            continue;
        }
        for (i = 0; i < a->nglobals; i++) {
            if (strcmp(a->globals[i].name, name) == 0) {
                value = a->globals[i].value;
                break;
            }
        }
        if (!s->defined || s->value != value) {
            a->changed = true;
        }
        s->ext = true;
        s->ext_line = a->line_no;
        s->defined = true;
        s->is_equ = true;
        s->value = value;
        s->kind = REL_EXT;
        snprintf(s->ext_of, sizeof s->ext_of, "%s", name);
    }
}

/* ENT records the module's address for a label. Its value is resolved after
 * pass 2, once the label's definition is known. */
static void ent_directive(Asm *a, const SrcLine *ln)
{
    const char *p;
    char name[ASM_NAME_MAX + 1];

    if (a->pass != 2) {
        return;
    }
    if (!ln->has_operand) {
        diag(a, "ENT needs a label name");
        return;
    }
    p = ln->operand;
    while (*p != '\0') {
        if (!next_name(&p, name, sizeof name)) {
            diag(a, "bad ENT operand '%s'", ln->operand);
            return;
        }
        if (name[0] == ':') {
            continue;
        }
        if (!name_list_add(&a->ents, name, a->pc, a->line_no)) {
            diag(a, "out of memory");
            return;
        }
    }
}

/* After pass 2: each ENT becomes an export, and each EXT the module read
 * becomes an import. */
static void collect_links(Asm *a)
{
    size_t i;

    for (i = 0; i < a->ents.n; i++) {
        const AsmName *e = &a->ents.items[i];
        const Symbol *s = sym_find(a, e->name);
        int32_t value = e->value;

        if (s != NULL && s->ext && s->ext_label_line != 0) {
            report(a, RANK_DUP_LABEL, "We have found 2 External Labels with the same name '%s' (File '%s', Line %d)",
                e->name, a->path, s->ext_label_line);
            return;
        }
        if (s != NULL && !s->ext && s->defined) {
            value = s->value;
        } else {
            warn(a, "ENT '%s' has no label in this file; exporting the address of the ENT line", e->name);
        }
        if (!name_list_add(&a->exports, e->name, value, e->line)) {
            diag(a, "out of memory");
            return;
        }
    }
    for (i = 0; i < a->nsyms; i++) {
        const Symbol *s = &a->syms[i];

        if (s->ext && s->ext_used && !name_list_add(&a->imports, s->key, 0, s->ext_line)) {
            diag(a, "out of memory");
            return;
        }
    }
}

/* merlin32 accepts these and ignores them, including any operand. The DAT
 * row itself is listed as a directive; its synthetic ASC line does the work. */
static bool noop_directive(const char *name)
{
    static const char *const names[] = {
        "ANOP", "START", "USING", "AST", "CYC", "LSTDO", "TTL", "SKP", "PAU", "SW", "USR", "DAT",
    };
    size_t i;

    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(name, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void pseudo_op(Asm *a, const char *name, const SrcLine *ln)
{
    a->ctx = directive_ctx(name);
    if (strcmp(name, "ORG") == 0) {
        org_directive(a, ln);
    } else if (strcmp(name, "DS") == 0) {
        ds_directive(a, ln);
    } else if (strcmp(name, "DFB") == 0 || strcmp(name, "DB") == 0) {
        data_directive(a, ln, 1, false);
    } else if (strcmp(name, "DW") == 0 || strcmp(name, "DA") == 0) {
        data_directive(a, ln, 2, false);
    } else if (strcmp(name, "DDB") == 0) {
        data_directive(a, ln, 2, true);
    } else if (strcmp(name, "CHK") == 0) {
        chk_directive(a);
    } else if (strcmp(name, "HEX") == 0) {
        hex_directive(a, ln);
    } else if (strcmp(name, "ASC") == 0) {
        text_directive(a, ln, TEXT_ASC);
    } else if (strcmp(name, "DCI") == 0) {
        text_directive(a, ln, TEXT_DCI);
    } else if (strcmp(name, "STR") == 0) {
        text_directive(a, ln, TEXT_STR);
    } else if (strcmp(name, "STRL") == 0) {
        text_directive(a, ln, TEXT_STRL);
    } else if (strcmp(name, "INV") == 0) {
        text_directive(a, ln, TEXT_INV);
    } else if (strcmp(name, "FLS") == 0) {
        text_directive(a, ln, TEXT_FLS);
    } else if (strcmp(name, "REV") == 0) {
        text_directive(a, ln, TEXT_REV);
    } else if (strcmp(name, "ADR") == 0) {
        data_directive(a, ln, 3, false);
    } else if (strcmp(name, "ADRL") == 0) {
        data_directive(a, ln, 4, false);
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
    } else if (strcmp(name, "LST") == 0 || strcmp(name, "PAG") == 0
        || strcmp(name, "TR") == 0 || strcmp(name, "EXP") == 0) {
        /* Listing controls. They change only merlin32's listing, not the binary. */
    } else if (strcmp(name, "XC") == 0) {
        /* merlin32 accepts XC and does not enforce 65C02 opcodes without it. */
    } else if (noop_directive(name)) {
        /* Accepted with no effect; see noop_directive. */
    } else if (strcmp(name, "ERR") == 0) {
        err_directive(a, ln);
    } else if (strcmp(name, "SAV") == 0 || strcmp(name, "DSK") == 0) {
        name_directive(a, ln);
    } else if (strcmp(name, "EXT") == 0) {
        ext_directive(a, ln);
    } else if (strcmp(name, "ENT") == 0) {
        ent_directive(a, ln);
    } else if (strcmp(name, "REL") == 0) {
        rel_directive(a, ln);
    } else if (strcmp(name, "TYP") == 0) {
        typ_directive(a, ln);
    } else {
        unknown_line(a);
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

static bool listing_control(const char *name)
{
    return strcmp(name, "LST") == 0 || strcmp(name, "PAG") == 0
        || strcmp(name, "TR") == 0 || strcmp(name, "EXP") == 0;
}

static bool directive_known(const char *name)
{
    static const char *const names[] = {
        "ORG", "DS", "DFB", "DB", "DW", "DA", "DDB", "CHK", "HEX", "ASC", "DCI", "STR", "STRL", "INV", "FLS", "REV",
        "ADR", "ADRL", "DUM", "DEND", "END", "MX", "LONGA", "LONGI", "LST", "PAG", "TR", "EXP",
        "ERR", "XC", "SAV", "DSK", "TYP", "EQU", "=", "KBD", "EXT", "ENT", "REL",
        "DO", "IF", "ELSE", "FIN",
    };
    size_t i;

    if (noop_directive(name)) {
        return true;
    }
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
    unknown_line(a);
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
            report(a, RANK_FIN, "FIN directive found in source file '%s' at line %d doesn't match with a previous (missing) DO or IF.",
                a->path, a->line_no);
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
        } else if (strcmp(name, "IF") == 0 && strpbrk(ln->operand, "<>=") == NULL && !mentions_mx(ln->operand)) {
            /* merlin32 skips a bare IF value unless it is an MX expression. */
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

/* Takes ownership of lines and org on success. */
static bool push_frame(Asm *a, char **lines, LineOrigin *org, size_t nlines)
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
    f->org = org;
    f->nlines = nlines;
    f->next = 0;
    return true;
}

static void pop_frame(Asm *a)
{
    Frame *f = cur_frame(a);

    free_lines(f->lines, f->nlines);
    free(f->org);
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
 * reported and kept as an empty line. ends_nl reports whether the last line
 * was terminated by a newline. */
static bool read_lines(Asm *a, FILE *f, char ***lines, size_t *nlines, bool *ends_nl)
{
    char buf[ASM_LINE_MAX + 2];
    char **out = NULL;
    size_t n = 0;
    size_t cap = 0;
    bool nl = false;

    while (fgets(buf, sizeof buf, f) != NULL) {
        size_t len = strlen(buf);
        int ch;

        nl = false;
        if (len > 0 && buf[len - 1] == '\n') {
            buf[--len] = '\0';
            nl = true;
        } else if (!feof(f)) {
            a->line_no = (int)(n + 1);
            diag(a, "line too long");
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {
                ;
            }
            nl = ch == '\n';
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
    *ends_nl = nl;
    return true;
}

static bool load_source(Asm *a, char ***lines, size_t *nlines, bool *ends_nl)
{
    FILE *f = fopen(a->path, "r");

    if (f == NULL) {
        record(a, RANK_OPEN, "Impossible to open Source file '%s'.", a->path);
        return false;
    }
    return read_lines(a, f, lines, nlines, ends_nl);
}

/* Name PUT and USE open: three or more characters with no '.' in the last
 * path component get ".s"; anything else is used as written. */
static bool include_name(const char *operand, char *out, size_t cap)
{
    size_t n = strlen(operand);
    const char *base = strrchr(operand, '/');
    int len;

    base = base ? base + 1 : operand;
    if (n >= 3 && strchr(base, '.') == NULL) {
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

/* Compares two file names, ignoring ASCII case. */
static bool name_eq_ci(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return false;
        }
    }
    return *a == *b;
}

/* Opens dir/name. If the exact name is missing, a file whose name differs
 * only in case is used, as in merlin32. */
static FILE *open_in_dir(const char *dir, const char *name)
{
    char path[INCLUDE_PATH_MAX];
    struct dirent *e;
    DIR *d;
    FILE *f = NULL;
    int len;

    len = snprintf(path, sizeof path, "%s/%s", dir, name);
    if (len < 0 || (size_t)len >= sizeof path) {
        return NULL;
    }
    f = fopen(path, "r");
    if (f != NULL) {
        return f;
    }
    d = opendir(dir);
    if (d == NULL) {
        return NULL;
    }
    while (f == NULL && (e = readdir(d)) != NULL) {
        if (name_eq_ci(e->d_name, name)) {
            len = snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            if (len >= 0 && (size_t)len < sizeof path) {
                f = fopen(path, "r");
            }
        }
    }
    closedir(d);
    return f;
}

/* Growing list of source lines, each with its listing origin. */
typedef struct {
    char **lines;
    LineOrigin *org;
    size_t n;
    size_t cap;
} Source;

static void source_free(Source *s)
{
    free_lines(s->lines, s->n);
    free(s->org);
    s->lines = NULL;
    s->org = NULL;
    s->n = 0;
    s->cap = 0;
}

static bool source_add(Source *s, const char *text, LineOrigin org)
{
    size_t len = strlen(text);
    char *copy;

    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        char **grown = realloc(s->lines, cap * sizeof *grown);
        LineOrigin *grown_org;

        if (grown == NULL) {
            return false;
        }
        s->lines = grown;
        grown_org = realloc(s->org, cap * sizeof *grown_org);
        if (grown_org == NULL) {
            return false;
        }
        s->org = grown_org;
        s->cap = cap;
    }
    copy = malloc(len + 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, text, len + 1);
    s->lines[s->n] = copy;
    s->org[s->n] = org;
    s->n++;
    return true;
}

static bool splice_lines(Asm *a, char **lines, size_t nlines, bool ends_nl, int file,
                         bool macro_only, bool expand, Source *out);

/* Opens a PUT or USE file, gives it the next listing file index, and adds its
 * lines. A USE file found only in the macro directory is macro-only. */
static bool include_file(Asm *a, const char *operand, bool use, Source *out)
{
    char name[INCLUDE_PATH_MAX];
    char dir[INCLUDE_PATH_MAX];
    char **lines = NULL;
    size_t nlines = 0;
    bool ends_nl = false;
    bool macro_only = false;
    FILE *f = NULL;
    bool ok;

    if (operand[0] == '\0') {
        return true;
    }
    if (include_name(operand, name, sizeof name)) {
        if (name[0] == '/') {
            f = fopen(name, "r");
        } else if (dir_of(a->path, dir, sizeof dir)) {
            f = open_in_dir(dir, name);
        }
        if (f == NULL && use && a->macro_dir != NULL && name[0] != '/') {
            f = open_in_dir(a->macro_dir, name);
            macro_only = f != NULL;
        }
    }
    if (f == NULL) {
        record(a, RANK_OPEN, "Impossible to open Source file '%s'.", name);
        return true;
    }
    if (!read_lines(a, f, &lines, &nlines, &ends_nl)) {
        return false;
    }
    listing_set_name(&a->lst, ++a->nfiles, name);
    ok = splice_lines(a, lines, nlines, ends_nl, a->nfiles, macro_only, false, out);
    free_lines(lines, nlines);
    return ok;
}

/* Formats one DAT selector (1..8) from the clock captured at startup. */
static void dat_format(time_t started, int sel, char *out, size_t sz)
{
    static const char *const months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
    };
    const struct tm *t = localtime(&started);
    int year;
    int hour;
    const char *meridiem;

    if (t == NULL) {
        out[0] = '\0';
        return;
    }
    year = t->tm_year + 1900 - 2000;
    hour = t->tm_hour > 12 ? t->tm_hour - 12 : t->tm_hour;
    meridiem = t->tm_hour > 12 ? "PM" : "AM";
    switch (sel) {
    case 1:
    case 5:
        snprintf(out, sz, "%02d-%s-%02d", t->tm_mday, months[t->tm_mon], year);
        break;
    case 2:
    case 6:
        snprintf(out, sz, "%02d/%02d/%02d", t->tm_mon + 1, t->tm_mday, year);
        break;
    case 3:
    case 7:
        snprintf(out, sz, "%02d-%s-%02d  %2d:%02d:%02d %s", t->tm_mday, months[t->tm_mon], year,
                 hour, t->tm_min, t->tm_sec, meridiem);
        break;
    default:
        snprintf(out, sz, "%02d/%02d/%02d  %2d:%02d:%02d %s", t->tm_mon + 1, t->tm_mday, year,
                 hour, t->tm_min, t->tm_sec, meridiem);
        break;
    }
}

/* DAT with a selector of 1..8 adds a synthetic ASC row after itself. Other
 * operands, and any text after the selector that is not a ';' comment, are
 * ignored, as merlin32 does. */
static bool dat_row(Asm *a, const SrcLine *ln, LineOrigin org, Source *out)
{
    char body[48];
    char text[ASM_LINE_MAX];
    int sel;
    char quote;

    if (!ln->has_operand || ln->trailing || ln->operand[0] < '1' || ln->operand[0] > '8'
        || ln->operand[1] != '\0') {
        return true;
    }
    sel = ln->operand[0] - '0';
    dat_format(a->started, sel, body, sizeof body);
    quote = sel <= 4 ? '"' : '\'';
    snprintf(text, sizeof text, " ASC  %c%s%c", quote, body, quote);
    return source_add(out, text, org);
}

static bool read_all(FILE *f, uint8_t **data, size_t *len)
{
    size_t cap = 0;

    *data = NULL;
    *len = 0;
    for (;;) {
        uint8_t chunk[4096];
        size_t n = fread(chunk, 1, sizeof chunk, f);
        uint8_t *grown;

        if (n == 0) {
            break;
        }
        if (*len + n > cap) {
            size_t ncap = cap ? cap : 4096;

            while (ncap < *len + n) {
                ncap *= 2;
            }
            grown = realloc(*data, ncap);
            if (grown == NULL) {
                free(*data);
                *data = NULL;
                return false;
            }
            *data = grown;
            cap = ncap;
        }
        memcpy(*data + *len, chunk, n);
        *len += n;
    }
    return !ferror(f);
}

/* PUTBIN lists the file's bytes as HEX rows of 16 bytes under a file index of
 * their own, then an empty row, as merlin32 does. The file is read from the
 * folder of the main source. */
static bool putbin_rows(Asm *a, const char *operand, Source *out)
{
    char dir[INCLUDE_PATH_MAX];
    char path[INCLUDE_PATH_MAX];
    uint8_t *data = NULL;
    size_t len = 0;
    size_t i;
    int file;
    FILE *f = NULL;
    LineOrigin org;
    bool ok = true;

    if (operand[0] == '\0') {
        return true;
    }
    if (dir_of(a->path, dir, sizeof dir) && snprintf(path, sizeof path, "%s/%s", dir, operand) < (int)sizeof path) {
        f = fopen(path, "rb");
    }
    if (f == NULL) {
        record(a, RANK_OPEN, "Impossible to open Binary file '%s'.", operand);
        return true;
    }
    ok = read_all(f, &data, &len);
    fclose(f);
    if (!ok) {
        free(data);
        return false;
    }
    file = ++a->nfiles;
    listing_set_name(&a->lst, file, operand);
    for (i = 0; i < len; i += 16) {
        char text[ASM_LINE_MAX];
        size_t j;
        size_t n = len - i < 16 ? len - i : 16;
        int pos;

        pos = snprintf(text, sizeof text, " HEX  ");
        for (j = 0; j < n; j++) {
            pos += snprintf(text + pos, sizeof text - (size_t)pos, "%s%02X", j ? "," : "", data[i + j]);
        }
        org.file = file;
        org.fline = (int)(i / 16) + 1;
        org.silent = false;
        if (!source_add(out, text, org)) {
            ok = false;
            break;
        }
    }
    if (ok) {
        org.file = file;
        org.fline = (int)((len + 15) / 16) + 1;
        org.silent = false;
        ok = source_add(out, "", org);
    }
    free(data);
    return ok;
}

/* Appends the lines of one file. A PUT, USE, or PUTBIN line outside a MAC body is kept
 * as a no-op directive row. When expand is set (the main source), the file it
 * names is spliced in right after that line, so included lines take part in
 * macros, LUP, and conditionals as if written in place. Included files keep
 * their PUT and USE lines as rows but do not open the files they name. A
 * macro-only file keeps just its MAC...EOM blocks, which are silent. A file
 * ending in a newline gets a final empty row, as merlin32 lists it. */
static bool splice_lines(Asm *a, char **lines, size_t nlines, bool ends_nl, int file,
                         bool macro_only, bool expand, Source *out)
{
    bool in_mac = false;
    size_t i;

    for (i = 0; i < nlines; i++) {
        SrcLine ln;
        const char *err = NULL;
        bool has_op = parse_line(lines[i], &ln, &err) && ln.has_opcode;
        bool is_eom = has_op && is_macro_end(&ln);
        LineOrigin org;

        org.file = file;
        org.fline = (int)(i + 1);
        org.silent = macro_only;
        if (expand) {
            a->line_no = (int)(i + 1);
        }
        if (has_op && (word_is(ln.opcode, "PUT") || word_is(ln.opcode, "USE") || word_is(ln.opcode, "PUTBIN"))) {
            if (macro_only) {
                continue;
            }
            if (!in_mac && !source_add(out, lines[i], org)) {
                return false;
            }
            if (word_is(ln.opcode, "PUTBIN")) {
                if (expand && !in_mac && !putbin_rows(a, ln.has_operand ? ln.operand : "", out)) {
                    return false;
                }
            } else if (expand && !include_file(a, ln.has_operand ? ln.operand : "", word_is(ln.opcode, "USE"), out)) {
                return false;
            }
            continue;
        }
        if (has_op && word_is(ln.opcode, "MAC")) {
            in_mac = true;
        } else if (is_eom) {
            in_mac = false;
        }
        if (macro_only && !in_mac && !is_eom) {
            continue;
        }
        if (!source_add(out, lines[i], org)) {
            return false;
        }
        if (has_op && !in_mac && word_is(ln.opcode, "DAT") && !dat_row(a, &ln, org, out)) {
            return false;
        }
    }
    if (ends_nl && nlines > 0 && !macro_only) {
        LineOrigin org;

        org.file = file;
        org.fline = (int)nlines + 1;
        org.silent = false;
        return source_add(out, "", org);
    }
    return true;
}

/* Splices the PUT and USE files into the main source. The main file is file 1. */
static bool expand_includes(Asm *a, char **lines, size_t nlines, bool ends_nl, Source *out)
{
    const char *slash = strrchr(a->path, '/');
    bool ok;

    a->nfiles = 1;
    listing_set_name(&a->lst, 1, slash ? slash + 1 : a->path);
    ok = splice_lines(a, lines, nlines, ends_nl, 1, false, true, out);
    if (!ok) {
        source_free(out);
    }
    return ok;
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
        /* merlin32 keeps the last definition. */
        warn(a, "Macro : '%s' found in '%s' and '%s'", name, m->file, a->path);
    }
    free_lines(m->lines, m->nlines);
    m->lines = NULL;
    m->nlines = 0;
    m->caplines = 0;
    m->def_run = a->run;
    snprintf(m->file, sizeof m->file, "%s", a->path);
    if (a->nopen == a->capopen) {
        size_t cap = a->capopen ? a->capopen * 2 : 4;
        size_t *grown = realloc(a->open_macs, cap * sizeof *grown);

        if (grown == NULL) {
            fprintf(stderr, "out of memory\n");
            return;
        }
        a->open_macs = grown;
        a->capopen = cap;
    }
    if (a->nopen == 0) {
        a->def_listed = cond_active(a);
    }
    a->open_macs[a->nopen++] = (size_t)(m - a->macros);
}

/* Stores one line of a MAC body. A MAC header while bodies are open stacks a
 * new macro on them; EOM closes all of them. An EOM label becomes an empty
 * line at the end of every closing body. */
static void record_macro_line(Asm *a, const char *text)
{
    SrcLine ln;
    const char *err = NULL;
    bool parsed = parse_line(text, &ln, &err);
    size_t i;

    a->lst_vis = a->def_listed ? LV_SHOW : LV_SKIP;
    a->lst_type = LST_DIRECTIVE;
    if (parsed) {
        a->lst_ln = ln;
    }
    if (parsed && is_macro_end(&ln)) {
        for (i = 0; ln.has_label && i < a->nopen; i++) {
            Macro *m = &a->macros[a->open_macs[i]];

            if (!append_line(&m->lines, &m->nlines, &m->caplines, ln.label)) {
                fprintf(stderr, "out of memory\n");
            }
        }
        a->nopen = 0;
        return;
    }
    if (parsed && ln.has_opcode && word_is(ln.opcode, "MAC")) {
        if (ln.has_label) {
            begin_macro(a, ln.label);
        } else {
            diag(a, "MAC needs a name");
        }
        return;
    }
    if (parsed && ln.has_label && ln.label[0] == ']') {
        a->ozunid++;
    }
    for (i = 0; i < a->nopen; i++) {
        Macro *m = &a->macros[a->open_macs[i]];

        if (!append_line(&m->lines, &m->nlines, &m->caplines, text)) {
            fprintf(stderr, "out of memory\n");
        }
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
    /* Written as a comparison, since a bare IF value is false (see conditional_op). */
    snprintf(line, sizeof line, "%s IF %d=1", ln.has_label ? ln.label : "",
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
    LineOrigin *org;
    size_t nmap = 0;
    size_t i;
    bool in_lup = a->lup_open && !a->lup_skip;
    bool first_lup = in_lup && a->lup_left == a->lup_count;

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
            if (in_lup) {
                a->ozb++;
                snprintf(map[nmap].to, sizeof map[nmap].to, "ozunid_%d", a->ozbase + a->ozb);
            } else {
                a->ozunid++;
                snprintf(map[nmap].to, sizeof map[nmap].to, "ozunid_%d", a->ozunid);
            }
            if (first_lup) {
                a->ozunid++;
            }
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
    org = malloc((nbody ? nbody : 1) * sizeof *org);
    if (org == NULL) {
        fprintf(stderr, "out of memory\n");
        free_lines(lines, nbody);
        return;
    }
    for (i = 0; i < nbody; i++) {
        org[i] = a->cur_org;
    }
    if (!push_frame(a, lines, org, nbody)) {
        free_lines(lines, nbody);
        free(org);
    }
}

/* NAME args calls a macro; PMC NAME calls it with no arguments. */
/* PMC and >>> name the macro as "name<delim>args". The name ends at the first
 * '.', ',', '/', '-', '(' or space. A space gives no arguments, as in merlin32. */
static bool explicit_call_op(const char *name)
{
    return strcmp(name, "PMC") == 0 || strcmp(name, ">>>") == 0;
}

static void macro_line(Asm *a, const SrcLine *ln, bool explicit_call)
{
    char name[ASM_NAME_MAX + 1];
    const char *target = ln->opcode;
    const char *args = explicit_call ? "" : ln->operand;
    bool has_args = ln->has_operand && !explicit_call;
    const Macro *m;

    if (explicit_call) {
        const char *p = ln->operand;
        size_t n = 0;

        if (!ln->has_operand) {
            diag(a, "PMC needs a macro name");
            return;
        }
        while (p[n] != '\0' && strchr(".,/-( ", p[n]) == NULL) {
            n++;
        }
        if (n >= sizeof name) {
            diag(a, "NOT MACRO");
            return;
        }
        memcpy(name, p, n);
        name[n] = '\0';
        target = name;
        if (p[n] != '\0' && p[n] != ' ') {
            args = p + n + 1;
            has_args = true;
        }
    }
    m = find_macro(a, target);
    if (m == NULL) {
        diag(a, "NOT MACRO");
        return;
    }
    if (ln->has_label) {
        define_label(a, ln->label, a->pc, true, false);
    }
    call_macro(a, m, args, has_args);
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
    a->ozb++;
    snprintf(ln->label, sizeof ln->label, "ozunid_%d", a->ozbase + a->ozb);
    if (a->lup_count > 26 && a->lup_left == a->lup_count) {
        diag(a, "Impossible to update Label '@' (more than 26 counts in Lup)");
    }
}

/* Pseudo-ops listed as data rows. The DUM ones list their bytes as zero. */
static bool data_op(const char *name)
{
    static const char *const ops[] = {"DFB", "DB", "DA", "DW", "ADR", "ADRL", "DS", "ASC",
                                      "DCI", "STR", "STRL", "DDB", "CHK", "FLS", "HEX", "INV", "REV"};
    size_t i;

    for (i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        if (strcmp(name, ops[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool dum_zero_op(const char *name)
{
    static const char *const ops[] = {"DFB", "DB", "DA", "DW", "ADR", "ADRL", "DS"};
    size_t i;

    for (i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        if (strcmp(name, ops[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void assemble_statement(Asm *a, const char *text);

/* Appends one operand as merlin32 lists it. An EQU or = name shows its value
 * as {$value}, with two hex digits up to $FF, four up to $FFFF, and six
 * above. A :local name shows as _local. Quoted text, numbers, and ]variables
 * are copied as written. */
/* Listing name for a ] code label, numbered on first sight in pass 2. */
static const char *oz_name(Asm *a, Symbol *s)
{
    if (s->oz_gen != a->ozgen) {
        s->oz_gen = a->ozgen;
        a->ozunid++;
        snprintf(s->oz_name, sizeof s->oz_name, "ozunid_%d", a->ozunid);
    }
    return s->oz_name;
}

/* A '*' is the program counter only where an operand term may begin. */
static bool pc_operand_pos(const char *op, const char *star)
{
    const char *q = star;

    while (q > op && (q[-1] == ' ' || q[-1] == '\t')) {
        q--;
    }
    return q == op || strchr("#([,+-<>", q[-1]) != NULL;
}

/* Writes the operand as the listing shows it. A '*' program counter gets its
 * own Empty row, labelled ozunid_N at the pre-line PC, before the instruction row. */
static void list_operand(Asm *a, const ListRow *row, const char *op, Buf *out)
{
    const char *p = op;

    while (*p != '\0') {
        const char *start = p;

        if (*p == '\'' || *p == '"') {
            char quote = *p++;

            while (*p != '\0' && *p != quote) {
                p++;
            }
            if (*p == quote) {
                p++;
            }
            buf_add(out, start, (size_t)(p - start));
        } else if (*p == '$' || *p == '%' || isdigit((unsigned char)*p)) {
            p++;
            while (isalnum((unsigned char)*p)) {
                p++;
            }
            buf_add(out, start, (size_t)(p - start));
        } else if (*p == ']' || name_start((unsigned char)*p) ||
                   (*p == ':' && name_start((unsigned char)p[1]))) {
            char name[ASM_LINE_MAX];
            char key[SYM_KEY_MAX];
            Symbol *s;
            size_t n;

            if (*p == ']') {
                p++;
            }
            if (*p == ':') {
                p++;
            }
            while (name_char((unsigned char)*p)) {
                p++;
            }
            n = (size_t)(p - start);
            if (n >= sizeof name) {
                buf_add(out, start, n);
                continue;
            }
            memcpy(name, start, n);
            name[n] = '\0';
            s = NULL;
            if (make_key(a, name, key, sizeof key)) {
                s = sym_find(a, key);
            }
            if (s != NULL && name[0] == ']') {
                if (s->oz_label) {
                    const char *z = oz_name(a, s);

                    buf_add(out, z, strlen(z));
                } else if (s->defined && a->lup_open) {
                    char value[16];

                    int n2 = snprintf(value, sizeof value, "%ld", (long)s->value);

                    buf_add(out, value, (size_t)n2);
                } else {
                    buf_add(out, name, n);
                }
            } else if (s != NULL && s->defined && s->is_equ) {
                if (s->disp != NULL) {
                    buf_add(out, s->disp, strlen(s->disp));
                } else {
                    char value[32];
                    uint32_t u = (uint32_t)s->value & 0xFFFFFF;

                    snprintf(value, sizeof value, "{$%0*lX}", u <= 0xFF ? 2 : u <= 0xFFFF ? 4 : 6,
                             (unsigned long)u);
                    buf_add(out, value, strlen(value));
                }
            } else if (s != NULL && name[0] == ':' && s->shown[0] != '\0') {
                buf_add(out, s->shown, strlen(s->shown));
            } else {
                buf_add(out, name, n);
            }
        } else if (*p == '*' && pc_operand_pos(op, p)) {
            char name[24];
            ListRow empty = *row;

            a->ozunid++;
            snprintf(name, sizeof name, "ozunid_%d", a->ozunid);
            empty.line = a->lst_seq++;
            empty.type = LST_EMPTY;
            empty.label = name;
            empty.opcode = NULL;
            empty.operand = NULL;
            empty.comment = NULL;
            empty.nbytes = 0;
            if (!listing_add_row(&a->lst, &empty)) {
                fprintf(stderr, "out of memory\n");
            }
            buf_add(out, name, strlen(name));
            p++;
        } else {
            buf_add(out, p, 1);
            p++;
        }
    }
}

/* Assembles one source line and, in pass 2 with a listing, records its row.
 * The row's bytes are the pool bytes from mark on; a skipped or hidden line
 * gives them back. */
static void assemble_line(Asm *a, const char *text, LineOrigin org)
{
    int32_t pc0 = a->pc;
    bool dum0 = a->dummy;
    size_t mark = a->lst.nbytes;
    ListRow row;
    char opc[OPCODE_MAX];
    char label_text[ASM_LINE_MAX + 2];
    Buf shown_op = {0};
    bool mx_unknown;

    a->lst_vis = LV_SKIP;
    a->lst_type = LST_EMPTY;
    a->lst_zero = false;
    memset(&a->lst_ln, 0, sizeof a->lst_ln);
    mx_unknown = a->ncond > 0 && !a->conds[a->ncond - 1];
    if (a->nopen > 0) {
        record_macro_line(a, text);
    } else {
        assemble_statement(a, text);
    }

    if (!a->listing) {
        return;
    }
    if (org.silent) {
        a->lst_vis = LV_SKIP;
    }
    if (a->lst_vis == LV_SKIP) {
        a->lst.nbytes = mark;
        return;
    }
    if (a->lst_vis == LV_HIDE) {
        a->lst.nbytes = mark;
        return;
    }
    if (a->lst_vis == LV_COUNT) {
        /* Hidden macro-expansion lines still take a line number in merlin32. */
        a->lst.nbytes = mark;
        a->lst_seq++;
        return;
    }
    a->lst_seq++;

    memset(&row, 0, sizeof row);
    row.file = org.file;
    row.fline = org.fline;
    row.type = a->lst_type;
    if ((dum0 || a->dummy) && a->lst_ln.has_opcode &&
        (row.type == LST_CODE || row.type == LST_DATA || row.type == LST_DIRECTIVE)) {
        row.type = LST_DUM;
    }
    row.pc = (!dum0 && a->dummy) ? a->pc : pc0;
    if (a->dummy && a->lst_ln.has_opcode && a->lst_ln.has_operand && word_is(a->lst_ln.opcode, "ORG")) {
        /* merlin32 lists an ORG inside DUM at $FFFF. */
        row.pc = 0xFFFF;
    }
    if (!a->dummy && a->lst_ln.has_opcode && !a->lst_ln.has_operand && word_is(a->lst_ln.opcode, "ORG")) {
        /* merlin32 lists a bare ORG at the location it resumes. */
        row.pc = a->pc;
    }
    if (mx_unknown) {
        strcpy(row.mx, "??");
    } else {
        row.mx[0] = a->acc16 ? '0' : '1';
        row.mx[1] = a->idx16 ? '0' : '1';
        row.mx[2] = '\0';
    }
    row.boff = mark;
    row.nbytes = a->lst.nbytes - mark;
    row.label = NULL;
    row.comment = (char *)a->lst_ln.comment;
    if (a->lst_ln.has_label) {
        if (a->lst_ln.label[0] == ':') {
            snprintf(label_text, sizeof label_text, "_%s", a->lst_ln.label + 1);
            row.label = label_text;
        } else {
            row.label = a->lst_ln.label;
            if (a->lst_ln.label[0] == ']') {
                Symbol *s = sym_find(a, a->lst_ln.label);

                if (s != NULL && s->oz_label) {
                    row.label = (char *)oz_name(a, s);
                }
            }
        }
    }
    if (a->lst_ln.has_opcode) {
        size_t olen = strlen(a->lst_ln.opcode);

        if (olen > 0 && a->lst_ln.opcode[olen - 1] == ':') {
            olen--;
        }
        memcpy(opc, a->lst_ln.opcode, olen);
        opc[olen] = '\0';
        row.opcode = opc;
    }
    if (a->lst_ln.has_operand) {
        buf_init(&shown_op);
        list_operand(a, &row, a->lst_ln.operand, &shown_op);
        row.operand = shown_op.oom ? a->lst_ln.operand : shown_op.s;
    }
    row.line = a->lst_seq;
    if (!listing_add_row(&a->lst, &row)) {
        fprintf(stderr, "out of memory\n");
    }
    free(shown_op.s);
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
    a->stmt = &ln;
    a->stmt_text = text;
    a->ctx = CTX_NONE;
    if (ln.has_label && cond_active(a)) {
        lup_label(a, &ln);
    }
    a->lst_ln = ln;
    if (!ln.has_opcode) {
        a->lst_vis = (a->lup_skip || !cond_active(a)) ? LV_SKIP : LV_SHOW;
        a->lst_type = ln.comment_line ? LST_COMMENT : LST_EMPTY;
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
            a->lst_type = LST_DIRECTIVE;
            a->lst_vis = cond_active(a) ? LV_SHOW : LV_SKIP;
            if (!ln.has_label) {
                diag(a, "MAC needs a name");
            } else {
                begin_macro(a, ln.label);
            }
        } else if (strcmp(name, "LUP") == 0) {
            lup_begin(a, &ln);
        } else if (is_conditional(name)) {
            a->lst_type = LST_DIRECTIVE;
            if (!cond_active(a) && (strcmp(name, "DO") == 0 || strcmp(name, "IF") == 0)) {
                a->lst_vis = LV_SKIP;
            } else if (a->nframes == 1) {
                a->lst_vis = LV_SHOW;
            } else if (ln.has_label) {
                a->lst_vis = LV_SHOW;
                a->lst_type = LST_EMPTY;
                a->lst_ln.has_opcode = false;
                a->lst_ln.has_operand = false;
            } else if (a->nframes > 1 && (strcmp(name, "FIN") == 0 || strcmp(name, "ELSE") == 0)) {
                a->lst_vis = LV_COUNT;
            } else if (a->nframes > 1 && cond_active(a)) {
                a->lst_vis = LV_COUNT;
            } else {
                a->lst_vis = LV_HIDE;
            }
            if (ln.has_label) {
                define_label(a, ln.label, a->pc, true, false);
            }
            conditional_op(a, name, &ln);
        } else if (strcmp(name, "PUT") == 0 || strcmp(name, "USE") == 0 || strcmp(name, "PUTBIN") == 0) {
            /* The named file's lines (or PUTBIN's HEX rows) follow this row; nothing is assembled here. */
            a->lst_type = LST_DIRECTIVE;
            if (cond_active(a)) {
                a->lst_vis = a->nframes == 1 ? LV_SHOW : LV_HIDE;
            }
        } else if (!cond_active(a)) {
            if (!explicit_call_op(name) && find_macro(a, ln.opcode) == NULL) {
                skipped_op(a, name);
            }
        } else if (explicit_call_op(name) || find_macro(a, ln.opcode) != NULL) {
            a->lst_type = LST_MACRO;
            if (a->nframes == 1 && !a->lup_open) {
                a->lst_vis = LV_SHOW;
            } else {
                a->lst_vis = (a->nframes > 1 || a->lup_open) ? LV_COUNT : LV_HIDE;
            }
            macro_line(a, &ln, explicit_call_op(name));
        } else if (strcmp(name, "EQU") == 0 || strcmp(name, "=") == 0) {
            int32_t v = 0;
            bool defined = false;
            bool var = ln.has_label && ln.label[0] == ']';
            ExprValue ev;

            a->lst_type = var ? LST_VARIABLE : LST_EQUIV;
            a->lst_vis = (a->lup_open || (var && a->nframes > 1)) ? LV_HIDE : LV_SHOW;
            if (!ln.has_label) {
                diag(a, "EQU needs a label");
            } else if (!ln.has_operand) {
                diag(a, "EQU needs a value");
            } else if (eval_operand_mode(a, ln.operand, !var, &ev)) {
                v = ev.value;
                defined = ev.defined;
                define_label_rel(a, ln.label, v, defined, true, &ev);
                set_equ_text(a, ln.label, ln.operand);
            }
        } else if (strcmp(name, "KBD") == 0) {
            /* merlin32 reads a key only interactively. With no key it sets
             * the label to the current PC, so the prompt operand is ignored. */
            a->lst_type = LST_DIRECTIVE;
            a->lst_vis = LV_SHOW;
            if (!ln.has_label) {
                warn(a, "KBD without a label emits no bytes");
            } else {
                define_label(a, ln.label, a->pc, true, false);
            }
        } else {
            a->lst_vis = LV_SHOW;
            if (ln.has_label) {
                define_label(a, ln.label, a->pc, true, false);
            }
            snprintf(base, sizeof base, "%.3s", name);
            if (len == 4 && name[3] != 'D' && opcode_known(base)) {
                forced = true;
                name[3] = '\0';
            }
            if (opcode_known(name)) {
                a->lst_type = LST_CODE;
                instruction(a, name, forced, &ln);
            } else {
                a->lst_type = data_op(name) ? LST_DATA : LST_DIRECTIVE;
                a->lst_zero = a->dummy && dum_zero_op(name);
                if (strcmp(name, "END") == 0) {
                    a->lst_vis = LV_SKIP;
                } else if ((a->nframes > 1 || a->lup_open) && listing_control(name)) {
                    /* merlin32 hides a listing control inside a macro or LUP
                     * expansion, but the hidden row still takes a line number. */
                    a->lst_vis = LV_COUNT;
                }
                pseudo_op(a, name, &ln);
            }
        }
    }
    a->stmt = NULL;
}

static bool run_pass(Asm *a, int pass)
{
    char **lines = NULL;
    size_t nlines = 0;
    bool ends_nl = false;
    Source src = {NULL, NULL, 0, 0};

    a->pass = pass;
    a->lst_seq = 0;
    a->ents.n = 0;
    listing_clear(&a->lst);
    a->nfiles = 0;
    a->line_no = 0;
    /* A standalone REL module is laid out from 0. bin->rel is set by an earlier run once REL is seen. */
    a->pc = (a->force_rel || (!a->linked && a->bin->rel)) ? 0 : a->start_pc;
    a->seg_base = a->pc;
    a->org_loc = a->pc;
    a->org_seen = false;
    a->scope[0] = '\0';
    a->ended = false;
    a->acc16 = false;
    a->idx16 = false;
    a->dummy = false;
    a->rel = a->force_rel;
    a->ncond = 0;
    a->cond_on = true;
    a->in_cond = false;
    a->lup_open = false;
    a->lup_skip = false;
    /* The first run has no previous total, so its phase B names sit far above
     * phase A names and cannot clash with them. Later runs reuse the same names. */
    a->ozbase = a->run == 0 ? OZ_FIRST_BASE : a->ozunid;
    a->ozunid = 0;
    a->ozb = 0;
    a->ozgen++;
    a->wide_seq = 0;
    a->run++;
    a->nopen = 0;

    while (a->nframes > 0) {
        pop_frame(a);
    }
    if (!load_source(a, &lines, &nlines, &ends_nl)) {
        return false;
    }
    if (!expand_includes(a, lines, nlines, ends_nl, &src)) {
        free_lines(lines, nlines);
        return false;
    }
    free_lines(lines, nlines);
    if (!push_frame(a, src.lines, src.org, src.n)) {
        source_free(&src);
        return false;
    }

    while (!a->ended) {
        Frame *f = cur_frame(a);
        LineOrigin org;
        const char *text;

        if (f->next >= f->nlines) {
            if (a->nframes == 1) {
                break;
            }
            pop_frame(a);
            continue;
        }
        org = f->org[f->next];
        text = f->lines[f->next++];
        if (a->nframes == 1) {
            a->line_no = (int)f->next;
            a->cur_org = org;
        }
        assemble_line(a, text, org);
    }
    while (a->nframes > 0) {
        pop_frame(a);
    }
    if (pass == 2 && a->ncond > 0) {
        report(a, RANK_FIN, "Missing FIN directive in source file '%s' (due to previous usage of a DO or IF directive).", a->path);
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
    free(a->open_macs);
}

static int cmp_numerical(const void *x, const void *y)
{
    const Symbol *p = *(const Symbol *const *)x;
    const Symbol *q = *(const Symbol *const *)y;

    if (p->is_equ != q->is_equ) {
        return p->is_equ ? -1 : 1;
    }
    if (p->value != q->value) {
        return p->value < q->value ? -1 : 1;
    }
    return p->def_order < q->def_order ? -1 : (p->def_order > q->def_order);
}

static int cmp_alphabetical(const void *x, const void *y)
{
    const Symbol *p = *(const Symbol *const *)x;
    const Symbol *q = *(const Symbol *const *)y;

    if (p->is_equ != q->is_equ) {
        return p->is_equ ? -1 : 1;
    }
    return strcmp(p->shown, q->shown);
}

/* Rows of `cols` entries; each entry is a 16-wide name, " =", and an 8-wide
 * value. The last row is not padded. */
static void print_rows(FILE *f, const Symbol **list, size_t n, size_t cols)
{
    size_t i;

    for (i = 0; i < n; i++) {
        const Symbol *s = list[i];
        char hex[8];
        const char *val = hex;

        if (s->is_equ && s->text != NULL) {
            val = s->text;
        } else {
            snprintf(hex, sizeof hex, "$%04X", (unsigned)s->value & 0xFFFFu);
        }
        fprintf(f, "%16s =%8s", s->shown, val);
        if ((i + 1) % cols == 0 || i + 1 == n) {
            fputc('\n', f);
        }
    }
}

/* Sorts all symbols with equates first, then prints the two groups. */
static void print_table(FILE *f, const char *title, const Symbol **all, size_t n,
    int (*cmp)(const void *, const void *), size_t cols)
{
    size_t split = 0;

    qsort(all, n, sizeof *all, cmp);
    while (split < n && all[split]->is_equ) {
        split++;
    }
    fprintf(f, "%s\n\n  Equates:\n", title);
    print_rows(f, all, split, cols);
    fprintf(f, "\n  Labels:\n");
    print_rows(f, all + split, n - split, cols);
    fputc('\n', f);
}

/* Appends merlin32-style symbol tables to the listing. Variables are left
 * out. cols is the number of entries per row; 0 means 6. */
static bool write_symbol_table(FILE *f, const Asm *a, size_t cols)
{
    const Symbol **all;
    size_t i, n = 0;

    all = malloc((a->nsyms + 1) * sizeof *all);
    if (all == NULL) {
        return false;
    }
    for (i = 0; i < a->nsyms; i++) {
        const Symbol *s = &a->syms[i];

        if (s->key[0] != ']' && s->defined && !s->ext) {
            all[n++] = s;
        }
    }
    if (cols == 0) {
        cols = 6;
    }
    fputc('\n', f);
    print_table(f, "Symbol Table - Numerical Order", all, n, cmp_numerical, cols);
    fputc('\n', f);
    print_table(f, "Symbol Table - Alphabetical Order", all, n, cmp_alphabetical, cols);
    free(all);
    return ferror(f) == 0;
}

/* Frees everything the assembler owns. Exports and imports must already be
 * moved out or cleared by the caller. */
static void asm_free(Asm *a)
{
    size_t i;

    free_macros(a);
    listing_free(&a->lst);
    for (i = 0; i < a->nsyms; i++) {
        free(a->syms[i].text);
        free(a->syms[i].disp);
    }
    free(a->syms);
    free(a->wide);
    free(a->conds);
    free(a->frames);
    free(a->ents.items);
    free(a->exports.items);
    free(a->imports.items);
}

/* Runs the sizing passes, then pass 2 and the link records. *pass2_started
 * says whether pass 2 ran, which is when a listing is written. */
static bool assemble_passes(Asm *a, bool *pass2_started)
{
    bool ok = true;

    for (a->iter = 1; ok; a->iter++) {
        a->changed = false;
        snapshot_vars(a);
        ok = run_pass(a, 1);
        if (ok && vars_changed(a)) {
            a->changed = true;
        }
        if (!ok || !a->changed) {
            break;
        }
        if (a->iter == MAX_LAYOUT_PASSES) {
            fprintf(stderr, "%s: layout does not settle after %d passes\n", a->path, MAX_LAYOUT_PASSES);
            ok = false;
        }
    }
    *pass2_started = ok;
    if (!ok) {
        return false;
    }
    ok = run_pass(a, 2);
    if (ok) {
        collect_links(a);
    }
    return ok && a->errors == 0;
}

bool assemble_source(const char *path, const char *macro_dir, const char *listing_stem, int table_cols, int console_cols, Binary *out)
{
    Asm a;
    FILE *listing = NULL;
    char lst_name[ASM_LINE_MAX + 16];
    bool ok;
    bool pass2_started;

    memset(&a, 0, sizeof a);
    a.started = time(NULL);
    out->bytes = NULL;
    out->len = 0;
    out->cap = 0;
    out->out_name[0] = '\0';
    out->file_type = 0;
    out->typed = false;
    out->rel = false;
    out->fix = NULL;
    out->nfix = 0;
    out->capfix = 0;
    a.path = path;
    a.macro_dir = macro_dir;
    a.listing = listing_stem != NULL;
    a.bin = out;
    a.start_pc = 0x8000;

    ok = assemble_passes(&a, &pass2_started);
    if (ok && a.imports.n > 0) {
        fprintf(stderr, "     => Error : Can't find External Label named '%s' (File '%s', Line %d).\n",
            a.imports.items[0].name, path, a.imports.items[0].line);
        ok = false;
    }
    if (a.err_has) {
        fprintf(stderr, "      => [Error] %s\n", a.err_msg);
    }
    if (listing_stem != NULL) {
        snprintf(lst_name, sizeof lst_name, "%s_Output.txt",
                 out->out_name[0] != '\0' ? out->out_name : listing_stem);
        listing = fopen(lst_name, "w");
        if (listing == NULL) {
            perror(lst_name);
            ok = false;
        }
    }
    if (pass2_started && listing != NULL) {
        listing_write(listing, &a.lst);
    }
    if (ok && table_cols >= 0 && listing != NULL && !write_symbol_table(listing, &a, (size_t)table_cols)) {
        fprintf(stderr, "%s: cannot write symbol table\n", path);
        ok = false;
    }
    if (ok && console_cols >= 0 && listing_stem != NULL &&
        !write_symbol_table(stdout, &a, (size_t)console_cols)) {
        fprintf(stderr, "%s: cannot write symbol table\n", path);
        ok = false;
    }
    if (listing != NULL && fclose(listing) != 0) {
        fprintf(stderr, "%s: cannot write %s\n", path, lst_name);
        ok = false;
    }

    asm_free(&a);
    if (!ok) {
        binary_free(out);
    }
    return ok;
}

bool assemble_module(const char *path, const char *macro_dir, int32_t start_pc, bool force_rel, const AsmName *globals, size_t nglobals, bool print_errors, AsmModule *out)
{
    Asm a;
    bool pass2_started;
    bool ok;

    memset(&a, 0, sizeof a);
    a.started = time(NULL);
    memset(out, 0, sizeof *out);
    a.path = path;
    a.macro_dir = macro_dir;
    a.bin = &out->bin;
    a.start_pc = start_pc;
    a.force_rel = force_rel;
    a.linked = true;
    a.globals = globals;
    a.nglobals = nglobals;
    a.quiet = !print_errors;

    ok = assemble_passes(&a, &pass2_started);
    if (!ok && a.err_has && print_errors) {
        fprintf(stderr, "      => [Error] %s\n", a.err_msg);
    }
    if (ok) {
        out->exports = a.exports.items;
        out->nexports = a.exports.n;
        a.exports.items = NULL;
        out->imports = a.imports.items;
        out->nimports = a.imports.n;
        a.imports.items = NULL;
    } else {
        binary_free(&out->bin);
    }
    asm_free(&a);
    return ok;
}

void module_free(AsmModule *m)
{
    binary_free(&m->bin);
    free(m->exports);
    free(m->imports);
    memset(m, 0, sizeof *m);
}


void binary_free(Binary *bin)
{
    free(bin->bytes);
    free(bin->fix);
    bin->bytes = NULL;
    bin->len = 0;
    bin->cap = 0;
    bin->fix = NULL;
    bin->nfix = 0;
    bin->capfix = 0;
}
