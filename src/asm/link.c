#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "fileinfo.h"
#include "line.h"
#include "link.h"
#include "omf.h"

/* Layout rounds stop once the exported values stop changing. A module's
 * labels depend on where earlier modules end, so a few rounds are normal. */
#define MAX_LINK_ROUNDS 32
#define TYP_FLAT 0x06
#define TYP_OMF 0xB3
#define ORG_MAX 0xFFFFFF
/* A flat module in a link with no DSK is assembled at this origin. */
#define NODSK_ORIGIN 0x8000
/* KIND of a segment with no KND: code, not absolute. */
#define KIND_DEFAULT 0x1000

typedef struct {
    char *name;
    char *path;
    bool has_org;
    /* Last ORG in the segment. Every module of the segment starts there plus
     * the bytes that earlier modules of the segment emitted. merlin32 does the
     * same for modules read before the ORG. */
    int32_t org;
    size_t first;
    size_t count;
} Segment;

typedef struct {
    char *shown;
    char *path;
    /* OMF attributes set by KND, ALI, LNA, and SNA. NULL names are blank. */
    uint16_t kind;
    uint32_t align;
    char *lname;
    char *sname;
    bool lna_set;
    bool sna_set;
} LinkModule;

typedef struct {
    char *dir;
    Segment *segs;
    size_t nsegs;
    size_t segcap;
    LinkModule *mods;
    size_t nmods;
    size_t modcap;
    /* Modules read before the first DSK. They come first in mods. Their
     * bytes are not written when a DSK exists. */
    size_t npre;
    /* An ASM has been read somewhere in the file. merlin32 needs one before any SNA. */
    bool asm_seen;
    /* Value of the last TYP in the file, or -1 when there is none. merlin32
     * decides the output type from that last TYP. */
    int typ;
    /* The output is an OMF file: one segment per module, no ORG. */
    bool omf;
    /* XPL was seen. An OMF link with more than one module then gets an
     * ExpressLoad segment in front of them. Flat links ignore it. */
    bool xpl;
    /* Low 16 bits of the last AUX, used as AuxType for every
     * _FileInformation.txt entry. */
    uint16_t aux;
    /* Keep each module's listing rows for its _Output.txt file (-v). */
    bool listing;
} Link;

enum { LS_NONE, LS_DSK, LS_ORG, LS_ASM };

static bool link_fail(const char *lk_path, int lineno, const char *msg)
{
    if (lineno > 0) {
        fprintf(stderr, "     => Error, Invalid Link file : %s (line %d).\n", msg, lineno);
    } else {
        fprintf(stderr, "     => Error, Invalid Link file : %s.\n", msg);
    }
    fprintf(stderr, "      => [Error] Impossible to load Link file '%s'.\n", lk_path);
    return false;
}

static void link_warn(int lineno, const char *msg)
{
    if (lineno > 0) {
        fprintf(stderr, "     => [Warning] Link file : %s (line %d).\n", msg, lineno);
    } else {
        fprintf(stderr, "     => [Warning] Link file : %s.\n", msg);
    }
}

static bool equal_ci(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return false;
        }
    }
    return *a == *b;
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);

    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

static char *dup_strn(const char *s, size_t len)
{
    char *p = malloc(len + 1);

    if (p != NULL) {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

/* Joins a file name to the link file's directory. An absolute name is kept. */
static char *join_dir(const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    char *p;

    if (name[0] == '/' || dl == 0) {
        return dup_str(name);
    }
    p = malloc(dl + strlen(name) + 1);
    if (p != NULL) {
        strcpy(p, dir);
        strcat(p, name);
    }
    return p;
}

static char *dir_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    size_t n = slash != NULL ? (size_t)(slash - path) + 1 : 0;
    char *d = malloc(n + 1);

    if (d != NULL) {
        memcpy(d, path, n);
        d[n] = '\0';
    }
    return d;
}

/* Grows a dynamic array so it can hold need items. */
static bool reserve(void **items, size_t *cap, size_t need, size_t elem)
{
    size_t ncap = *cap > 0 ? *cap : 8;
    void *p;

    if (need <= *cap) {
        return true;
    }
    while (ncap < need) {
        ncap *= 2;
    }
    p = realloc(*items, ncap * elem);
    if (p == NULL) {
        return false;
    }
    *items = p;
    *cap = ncap;
    return true;
}

/* Reads a text file into lines without line endings. A line longer than
 * ASM_LINE_MAX is cut short and sets *too_long. */
static bool load_lines(const char *path, char ***out, size_t *count, bool *too_long)
{
    FILE *f = fopen(path, "r");
    char buf[ASM_LINE_MAX + 2];
    char **lines = NULL;
    size_t n = 0;
    size_t cap = 0;
    bool ok = f != NULL;

    *too_long = false;
    while (ok && fgets(buf, sizeof buf, f) != NULL) {
        size_t len = strlen(buf);
        char *copy;

        if (len > 0 && buf[len - 1] == '\n') {
            buf[--len] = '\0';
        } else if (len == sizeof buf - 1 && !feof(f)) {
            int c;

            *too_long = true;
            while ((c = fgetc(f)) != '\n' && c != EOF) {
                /* Discard the rest of the long line. */
            }
        }
        if (len > 0 && buf[len - 1] == '\r') {
            buf[--len] = '\0';
        }
        copy = dup_str(buf);
        ok = copy != NULL && reserve((void **)&lines, &cap, n + 1, sizeof *lines);
        if (ok) {
            lines[n++] = copy;
        } else {
            free(copy);
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    if (!ok) {
        for (size_t i = 0; i < n; i++) {
            free(lines[i]);
        }
        free(lines);
        return false;
    }
    *out = lines;
    *count = n;
    return true;
}

static void free_text_lines(char **lines, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        free(lines[i]);
    }
    free(lines);
}

static bool no_symbols(void *ctx, const char *name, ExprValue *value)
{
    (void)ctx;
    (void)name;
    (void)value;
    return false;
}

/* Evaluates an operand that may use only numbers. */
static bool const_operand(const char *text, int32_t *value)
{
    ExprEnv env = {no_symbols, NULL, 0, false};
    ExprValue v;
    const char *err = NULL;

    if (!expr_eval(text, &env, &v, &err) || !v.defined) {
        return false;
    }
    *value = v.value;
    return true;
}

static bool add_segment(Link *lk, const SrcLine *ln)
{
    Segment *s;

    if (!reserve((void **)&lk->segs, &lk->segcap, lk->nsegs + 1, sizeof *lk->segs)) {
        return false;
    }
    s = &lk->segs[lk->nsegs];
    s->name = dup_str(ln->operand);
    s->path = s->name != NULL ? join_dir(lk->dir, ln->operand) : NULL;
    s->has_org = false;
    s->org = 0;
    s->first = lk->nmods;
    s->count = 0;
    if (s->name == NULL || s->path == NULL) {
        free(s->name);
        free(s->path);
        return false;
    }
    lk->nsegs++;
    return true;
}

static bool add_module(Link *lk, const SrcLine *ln)
{
    LinkModule *m;

    if (!reserve((void **)&lk->mods, &lk->modcap, lk->nmods + 1, sizeof *lk->mods)) {
        return false;
    }
    m = &lk->mods[lk->nmods];
    m->kind = KIND_DEFAULT;
    m->align = 0;
    m->lname = NULL;
    m->sname = NULL;
    m->lna_set = false;
    m->sna_set = false;
    m->shown = dup_str(ln->operand);
    m->path = m->shown != NULL ? join_dir(lk->dir, ln->operand) : NULL;
    if (m->shown == NULL || m->path == NULL) {
        free(m->shown);
        free(m->path);
        return false;
    }
    lk->nmods++;
    if (lk->nsegs == 0) {
        lk->npre++;
    } else {
        lk->segs[lk->nsegs - 1].count++;
    }
    return true;
}

/* Returns the operand of a directive line as written: spaces inside the name
 * are kept and a trailing comment is dropped. parse_line stops at a space, so
 * LNA and SNA read the rest of the line from the raw text. */
static char *raw_operand(const char *text)
{
    const char *p = text;
    const char *start;
    const char *end;
    char quote = 0;

    if (*p != ' ' && *p != '\t' && *p != '\0') {
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    start = p;
    for (; *p != '\0' && (quote != 0 || *p != ';'); p++) {
        if (quote != 0) {
            if (*p == quote) {
                quote = 0;
            }
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        }
    }
    end = p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    return dup_strn(start, (size_t)(end - start));
}

/* Removes one pair of matching surrounding quotes. Mismatched quotes stay. */
static void strip_quotes(char *s)
{
    size_t n = strlen(s);

    if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

/* Sets LNA or SNA on the last module. An identical repeat is ignored and a
 * different one is an error. */
static bool set_name(Link *lk, const char *lk_path, int lineno, const char *text, const char *op, bool is_sna)
{
    LinkModule *m = &lk->mods[lk->nmods - 1];
    char **field = is_sna ? &m->sname : &m->lname;
    bool *set = is_sna ? &m->sna_set : &m->lna_set;
    char *val = raw_operand(text);

    if (val == NULL) {
        return link_fail(lk_path, lineno, "out of memory");
    }
    strip_quotes(val);
    if (*set) {
        bool same = equal_ci(*field != NULL ? *field : "", val);

        free(val);
        if (same) {
            return true;
        }
        {
            char msg[ASM_LINE_MAX + 64];

            snprintf(msg, sizeof msg, "Two %s directives found for Segment '%s'", op, m->shown);
            return link_fail(lk_path, lineno, msg);
        }
    }
    *field = val;
    *set = true;
    return true;
}

/* KIND low bytes merlin32 accepts: $00, $01, $02, $04, $08, $10, $12. */
static bool valid_kind(uint8_t low)
{
    static const uint8_t ok[] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x12};
    size_t i;

    for (i = 0; i < sizeof ok; i++) {
        if (low == ok[i]) {
            return true;
        }
    }
    return false;
}

/* Value of one hex digit, or -1 when c is not one. */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Reads an AUX operand as merlin32 does. It never fails: text it cannot use
 * gives 0. A leading # is skipped. $ reads hex, % reads binary, and anything
 * else reads decimal. One sign may follow the base mark. In binary, + and -
 * are skipped wherever they appear and never negate. The result is the low
 * 16 bits. */
static uint16_t aux_value(const char *s)
{
    uint32_t v = 0;
    bool neg = false;
    int d;

    if (*s == '#') {
        s++;
    }
    if (*s == '$') {
        s++;
        if (*s == '-' || *s == '+') {
            neg = *s == '-';
            s++;
        }
        while ((d = hex_digit(*s)) >= 0) {
            v = v * 16 + (uint32_t)d;
            s++;
        }
    } else if (*s == '%') {
        s++;
        for (; *s == '0' || *s == '1' || *s == '+' || *s == '-'; s++) {
            if (*s == '0' || *s == '1') {
                v = v * 2 + (uint32_t)(*s - '0');
            }
        }
    } else {
        if (*s == '-' || *s == '+') {
            neg = *s == '-';
            s++;
        }
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            s++;
        }
    }
    if (neg) {
        v = 0u - v;
    }
    return (uint16_t)v;
}

/* Applies one directive to the link state. The normal order is DSK, ORG,
 * one or more ASM, SNA. After an SNA, ORG and ASM continue the previous file,
 * with a warning. ASM may come before the first DSK; those modules are
 * assembled but not written. check_mode decides whether ORG is required. */
static bool link_directive(Link *lk, const char *lk_path, int lineno, const SrcLine *ln, const char *text, int *state)
{
    const char *op = ln->opcode;
    int32_t v = 0;

    if (equal_ci(op, "TYP")) {
        if (!ln->has_operand || !const_operand(ln->operand, &v) || v < 0 || v > 0xFF) {
            return link_fail(lk_path, lineno, "TYP needs a constant from $00 to $FF");
        }
        return true;
    }
    if (equal_ci(op, "DSK")) {
        /* merlin32 accepts a DSK even when the previous segment has no SNA. */
        if (!ln->has_operand) {
            return link_fail(lk_path, lineno, "DSK needs a file name");
        }
        if (!add_segment(lk, ln)) {
            return link_fail(lk_path, lineno, "out of memory");
        }
        *state = LS_DSK;
        return true;
    }
    if (equal_ci(op, "ORG")) {
        if (lk->nsegs == 0) {
            return link_fail(lk_path, lineno, "ORG must follow DSK");
        }
        /* A bare ORG sets the origin to $0000, as merlin32 does. */
        if (!ln->has_operand) {
            v = 0;
        } else if (!const_operand(ln->operand, &v) || v < 0 || v > ORG_MAX) {
            return link_fail(lk_path, lineno, "ORG needs a constant address");
        }
        if (*state == LS_NONE) {
            link_warn(lineno, "ORG has no DSK, it continues the previous file");
        }
        lk->segs[lk->nsegs - 1].has_org = true;
        lk->segs[lk->nsegs - 1].org = v;
        *state = LS_ORG;
        return true;
    }
    if (equal_ci(op, "ASM")) {
        if (!ln->has_operand) {
            return link_fail(lk_path, lineno, "ASM needs a file name");
        }
        if (*state == LS_NONE && lk->nsegs > 0) {
            link_warn(lineno, "ASM has no ORG, it continues at the previous module's end");
        }
        if (!add_module(lk, ln)) {
            return link_fail(lk_path, lineno, "out of memory");
        }
        lk->asm_seen = true;
        *state = LS_ASM;
        return true;
    }
    if (equal_ci(op, "SNA")) {
        if (!lk->asm_seen) {
            return link_fail(lk_path, lineno, "SNA needs a previous ASM");
        }
        if (ln->has_operand && !set_name(lk, lk_path, lineno, text, "SNA", true)) {
            return false;
        }
        *state = LS_NONE;
        return true;
    }
    if (equal_ci(op, "KND") || equal_ci(op, "ALI") || equal_ci(op, "LNA")) {
        LinkModule *m;

        if (!lk->asm_seen) {
            char msg[OPCODE_MAX + 32];

            snprintf(msg, sizeof msg, "%s needs a previous ASM", op);
            return link_fail(lk_path, lineno, msg);
        }
        m = &lk->mods[lk->nmods - 1];
        if (equal_ci(op, "LNA")) {
            return set_name(lk, lk_path, lineno, text, "LNA", false);
        }
        if (equal_ci(op, "KND")) {
            if (ln->has_operand && !const_operand(ln->operand, &v)) {
                return link_fail(lk_path, lineno, "KND needs a constant");
            }
            if (!valid_kind((uint8_t)(v & 0xFF))) {
                return link_fail(lk_path, lineno, "KND needs a valid kind constant");
            }
            m->kind = (uint16_t)(v & 0xFFFF);
            return true;
        }
        if (ln->has_operand && equal_ci(ln->operand, "NONE")) {
            m->align = 0;
        } else if (ln->has_operand && equal_ci(ln->operand, "PAGE")) {
            m->align = 1;
        } else if (ln->has_operand && equal_ci(ln->operand, "BANK")) {
            m->align = 2;
        } else {
            return link_fail(lk_path, lineno, "ALI needs NONE, PAGE or BANK");
        }
        return true;
    }
    if (equal_ci(op, "XPL")) {
        lk->xpl = true;
        return true;
    }
    if (equal_ci(op, "AUX")) {
        lk->aux = aux_value(ln->has_operand ? ln->operand : "");
        return true;
    }
    {
        char msg[OPCODE_MAX + 48];

        snprintf(msg, sizeof msg, "unknown directive '%s'", op);
        return link_fail(lk_path, lineno, msg);
    }
}

/* Returns the value of the last TYP in the file, or -1 when there is none.
 * merlin32 decides the output type from the whole file, so this is checked
 * before any directive is applied. */
static int last_typ(char **lines, size_t n)
{
    size_t i;
    int last = -1;

    for (i = 0; i < n; i++) {
        SrcLine ln;
        const char *err = NULL;
        int32_t v = 0;

        if (parse_line(lines[i], &ln, &err) && ln.has_opcode && equal_ci(ln.opcode, "TYP") &&
            ln.has_operand && const_operand(ln.operand, &v) && v >= 0 && v <= 0xFF) {
            last = (int)v;
        }
    }
    return last;
}

/* Picks the output type once the whole file is read. A file with TYP $06
 * and no other TYP is flat: every DSK needs an ORG. Anything else is OMF: one
 * DSK, and no ORG. */
static bool check_mode(Link *lk, const char *lk_path)
{
    size_t s;

    if (lk->typ == TYP_FLAT) {
        for (s = 0; s < lk->nsegs; s++) {
            if (!lk->segs[s].has_org) {
                char msg[64];

                snprintf(msg, sizeof msg, "Directive ORG is missing for File #%lu", (unsigned long)(s + 1));
                return link_fail(lk_path, 0, msg);
            }
        }
        return true;
    }
    lk->omf = true;
    if (lk->nsegs == 0 && lk->nmods > 0) {
        return link_fail(lk_path, 0, "Directive DSK is missing (Target OMF File name)");
    }
    if (lk->nsegs > 1) {
        return link_fail(lk_path, 0, "Too many DSK directives (only one for an OMF File)");
    }
    for (s = 0; s < lk->nsegs; s++) {
        if (lk->segs[s].has_org) {
            return link_fail(lk_path, 0, "Directive ORG is not allowed (Target is a Relocatable OMF File)");
        }
    }
    return true;
}

static bool parse_link(const char *lk_path, Link *lk)
{
    char **lines = NULL;
    size_t nlines = 0;
    size_t i;
    bool too_long = false;
    int state = LS_NONE;
    bool ok = true;

    if (!load_lines(lk_path, &lines, &nlines, &too_long)) {
        fprintf(stderr, "asm816: cannot read %s\n", lk_path);
        return false;
    }
    if (too_long) {
        ok = link_fail(lk_path, 0, "a line is longer than 1024 characters");
    }
    lk->typ = last_typ(lines, nlines);
    for (i = 0; ok && i < nlines; i++) {
        SrcLine ln;
        const char *err = NULL;
        int lineno = (int)i + 1;

        if (!parse_line(lines[i], &ln, &err)) {
            ok = link_fail(lk_path, lineno, err);
        } else if (ln.has_opcode) {
            ok = link_directive(lk, lk_path, lineno, &ln, lines[i], &state);
        }
    }
    if (ok) {
        ok = check_mode(lk, lk_path);
    }
    if (ok) {
        if (state == LS_ORG) {
            link_warn(0, "the last ORG has no ASM and is ignored");
        } else if (state == LS_ASM) {
            link_warn(0, "the last segment has no SNA");
        }
    }
    free_text_lines(lines, nlines);
    return ok;
}


static bool check_sources(const Link *lk)
{
    size_t k;

    for (k = 0; k < lk->nmods; k++) {
        FILE *f = fopen(lk->mods[k].path, "r");

        if (f == NULL) {
            fprintf(stderr, "      => [Error] Impossible to load Source file '%s'.\n", lk->mods[k].shown);
            return false;
        }
        fclose(f);
    }
    return true;
}

/* Assembles one module at start. A link with no DSK has no output file to
 * place it in, so the start is NODSK_ORIGIN for those modules. */
static bool assemble_one(const Link *lk, size_t k, int32_t start, const char *macro_dir,
    const AsmName *globals, size_t nglobals, bool print, int *ozcarry, AsmModule *res)
{
    if (!assemble_module(lk->mods[k].path, macro_dir, start, lk->omf, globals, nglobals, print, lk->listing, *ozcarry, &res[k])) {
        return false;
    }
    *ozcarry = res[k].ozunid_end;
    if (print && (start & 0xFFFF) + (int32_t)res[k].bin.len > 0x10000) {
        fprintf(stderr, "     => Error : Object code size > 64 KB (file '%s').\n", lk->mods[k].shown);
        return false;
    }
    return true;
}

static bool assemble_all(const Link *lk, const char *macro_dir, const AsmName *globals,
    size_t nglobals, bool print, AsmModule *res)
{
    size_t s;
    size_t k;
    int32_t pre_start = lk->nsegs == 0 ? NODSK_ORIGIN : 0;
    int ozcarry = 0;

    for (k = 0; k < lk->npre; k++) {
        if (!assemble_one(lk, k, pre_start, macro_dir, globals, nglobals, print, &ozcarry, res)) {
            return false;
        }
    }
    for (s = 0; s < lk->nsegs; s++) {
        const Segment *seg = &lk->segs[s];
        int32_t emitted = 0;

        for (k = seg->first; k < seg->first + seg->count; k++) {
            /* Each OMF module is its own segment, laid out from 0. */
            int32_t start = lk->omf ? 0 : seg->org + emitted;

            if (!assemble_one(lk, k, start, macro_dir, globals, nglobals, print, &ozcarry, res)) {
                return false;
            }
            emitted += (int32_t)res[k].bin.len;
        }
    }
    return true;
}

static void free_modules(AsmModule *res, size_t n)
{
    size_t k;

    for (k = 0; k < n; k++) {
        module_free(&res[k]);
    }
}

/* Builds the export table in link order. The first module to export a name wins. */
static bool collect_globals(const Link *lk, const AsmModule *res, AsmName **out, size_t *nout)
{
    size_t total = 0;
    size_t k;
    size_t j;
    size_t n = 0;
    AsmName *g;

    for (k = 0; k < lk->nmods; k++) {
        total += res[k].nexports;
    }
    g = malloc((total > 0 ? total : 1) * sizeof *g);
    if (g == NULL) {
        return false;
    }
    for (k = 0; k < lk->nmods; k++) {
        for (j = 0; j < res[k].nexports; j++) {
            const AsmName *e = &res[k].exports[j];
            size_t x;
            bool seen = false;

            for (x = 0; x < n && !seen; x++) {
                seen = strcmp(g[x].name, e->name) == 0;
            }
            if (!seen) {
                g[n++] = *e;
            }
        }
    }
    *out = g;
    *nout = n;
    return true;
}

static bool same_globals(const AsmName *a, size_t na, const AsmName *b, size_t nb)
{
    size_t i;

    if (na != nb) {
        return false;
    }
    for (i = 0; i < na; i++) {
        if (a[i].value != b[i].value || strcmp(a[i].name, b[i].name) != 0) {
            return false;
        }
    }
    return true;
}

/* Repeats assembly until the exported values stop changing, then reruns
 * the modules once with their diagnostics. On return res holds the final
 * modules and *gout holds the converged exports. */
static bool run_layout(const Link *lk, const char *macro_dir, AsmModule *res, AsmName **gout, size_t *ngout)
{
    AsmName *globals = NULL;
    AsmName *next = NULL;
    size_t ng = 0;
    size_t nnext = 0;
    size_t round;
    bool settled = false;

    for (round = 0; !settled; round++) {
        if (round == MAX_LINK_ROUNDS) {
            fprintf(stderr, "asm816: link layout does not settle after %d rounds\n", MAX_LINK_ROUNDS);
            goto fail;
        }
        if (!assemble_all(lk, macro_dir, globals, ng, false, res)) {
            free_modules(res, lk->nmods);
            assemble_all(lk, macro_dir, globals, ng, true, res);
            free_modules(res, lk->nmods);
            goto fail;
        }
        if (!collect_globals(lk, res, &next, &nnext)) {
            fprintf(stderr, "out of memory\n");
            free_modules(res, lk->nmods);
            goto fail;
        }
        settled = same_globals(globals, ng, next, nnext);
        free(globals);
        globals = next;
        ng = nnext;
        next = NULL;
        free_modules(res, lk->nmods);
    }
    if (!assemble_all(lk, macro_dir, globals, ng, true, res)) {
        free_modules(res, lk->nmods);
        goto fail;
    }
    *gout = globals;
    *ngout = ng;
    return true;

fail:
    free(globals);
    free(next);
    return false;
}

static bool is_imported(const Link *lk, const AsmModule *res, const char *name)
{
    size_t k;
    size_t j;

    for (k = 0; k < lk->nmods; k++) {
        for (j = 0; j < res[k].nimports; j++) {
            if (strcmp(res[k].imports[j].name, name) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* merlin32 rejects a name that two modules export when some module imports it.
 * The error names the later module's label. */
static bool check_export_clashes(const Link *lk, const AsmModule *res)
{
    size_t k;
    size_t j;
    size_t m;
    size_t x;

    for (k = 0; k < lk->nmods; k++) {
        for (j = 0; j < res[k].nexports; j++) {
            const AsmName *e = &res[k].exports[j];

            for (m = 0; m < k; m++) {
                for (x = 0; x < res[m].nexports; x++) {
                    if (strcmp(res[m].exports[x].name, e->name) == 0 && is_imported(lk, res, e->name)) {
                        fprintf(stderr,
                            "     => Error : We have found 2 External Labels with the same name '%s' (File '%s', Line %d).\n",
                            e->name, lk->mods[k].shown, e->line);
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static bool check_imports(const Link *lk, const AsmModule *res, const AsmName *globals, size_t ng)
{
    size_t k;
    size_t j;
    size_t g;

    for (k = 0; k < lk->nmods; k++) {
        for (j = 0; j < res[k].nimports; j++) {
            const AsmName *imp = &res[k].imports[j];
            bool found = false;

            for (g = 0; g < ng && !found; g++) {
                found = strcmp(globals[g].name, imp->name) == 0;
            }
            if (!found) {
                fprintf(stderr, "     => Error : Can't find External Label named '%s' (File '%s', Line %d).\n",
                    imp->name, lk->mods[k].shown, imp->line);
                return false;
            }
        }
    }
    return true;
}

static bool write_segments(const Link *lk, const AsmModule *res)
{
    size_t s;

    for (s = 0; s < lk->nsegs; s++) {
        const Segment *seg = &lk->segs[s];
        FILE *f = fopen(seg->path, "wb");
        size_t k;
        bool ok = f != NULL;

        if (!ok) {
            perror(seg->name);
            return false;
        }
        for (k = seg->first; ok && k < seg->first + seg->count; k++) {
            const Binary *b = &res[k].bin;

            if (b->len > 0 && fwrite(b->bytes, 1, b->len, f) != b->len) {
                ok = false;
            }
        }
        if (fclose(f) != 0) {
            ok = false;
        }
        if (!ok) {
            fprintf(stderr, "asm816: cannot write %s\n", seg->name);
            return false;
        }
        printf("     => Creating Object file '%s'\n", seg->name);
    }
    return true;
}

/* merlin32 names a no-DSK module's file after the basename of its source,
 * without the text from the last dot. A leading dot counts, so ".s" gives an
 * empty name, which merlin32 cannot create. */
static char *module_out_name(const char *shown)
{
    const char *base = strrchr(shown, '/');
    const char *dot;

    base = base != NULL ? base + 1 : shown;
    dot = strrchr(base, '.');
    if (dot == NULL) {
        return dup_str(base);
    }
    return dup_strn(base, (size_t)(dot - base));
}

/* With no DSK, each flat module is written to its own file. A duplicate name
 * is overwritten, so the last module wins. */
static bool write_module_files(const Link *lk, const AsmModule *res)
{
    size_t k;

    for (k = 0; k < lk->nmods; k++) {
        char *name = module_out_name(lk->mods[k].shown);
        char *path = name != NULL ? join_dir(lk->dir, name) : NULL;
        const Binary *b = &res[k].bin;
        FILE *f;
        bool ok;

        if (name == NULL || path == NULL) {
            free(name);
            free(path);
            fprintf(stderr, "out of memory\n");
            return false;
        }
        if (name[0] == '\0') {
            fprintf(stderr, "asm816: Can't create Object file ''\n");
            free(name);
            free(path);
            continue;
        }
        f = fopen(path, "wb");
        ok = f != NULL && (b->len == 0 || fwrite(b->bytes, 1, b->len, f) == b->len);
        if (f != NULL && fclose(f) != 0) {
            ok = false;
        }
        if (!ok) {
            fprintf(stderr, "asm816: cannot write %s\n", name);
            free(name);
            free(path);
            return false;
        }
        printf("     => Creating Object file '%s'\n", name);
        free(name);
        free(path);
    }
    return true;
}

/* Finds the first module in link order that exports name and sets *mod to
 * its index. */
static bool find_provider(const Link *lk, const AsmModule *res, const char *name, size_t *mod)
{
    size_t k;
    size_t j;

    for (k = 0; k < lk->nmods; k++) {
        for (j = 0; j < res[k].nexports; j++) {
            if (strcmp(res[k].exports[j].name, name) == 0) {
                *mod = k;
                return true;
            }
        }
    }
    return false;
}

/* Writes the OMF link: one segment per module, numbered in link order. With
 * an ExpressLoad (XPL and more than one module) the ExpressLoad is SEGNUM 1
 * and the modules start at SEGNUM 2. A fixup that names an external label
 * refers to the provider's segment. */
static bool write_omf_link(const Link *lk, const AsmModule *res)
{
    OmfSegment *segs = NULL;
    OmfReloc **rels = NULL;
    uint8_t *out = NULL;
    size_t outlen = 0;
    const char *err = NULL;
    FILE *f;
    size_t k;
    size_t i;
    bool ok = false;
    bool express = lk->xpl && lk->nmods > 1;
    /* SEGNUM of the first module: the ExpressLoad is 1 when present. */
    int base = express ? 2 : 1;

    if (lk->nsegs == 0) {
        return true;
    }
    if (lk->nmods == 0) {
        f = fopen(lk->segs[0].path, "wb");
        if (f == NULL || fclose(f) != 0) {
            fprintf(stderr, "asm816: cannot write %s\n", lk->segs[0].name);
            return false;
        }
        printf("     => Creating OMF file '%s'\n", lk->segs[0].name);
        return true;
    }
    segs = calloc(lk->nmods, sizeof *segs);
    rels = calloc(lk->nmods, sizeof *rels);
    if (segs == NULL || rels == NULL) {
        fprintf(stderr, "out of memory\n");
        goto out;
    }
    for (k = 0; k < lk->nmods; k++) {
        const Binary *b = &res[k].bin;

        rels[k] = malloc((b->nfix > 0 ? b->nfix : 1) * sizeof **rels);
        if (rels[k] == NULL) {
            fprintf(stderr, "out of memory\n");
            goto out;
        }
        for (i = 0; i < b->nfix; i++) {
            const Fixup *fx = &b->fix[i];
            OmfReloc *r = &rels[k][i];

            r->offset = fx->offset;
            r->size = fx->size;
            r->shift = fx->shift;
            r->external = fx->external;
            r->seg = 0;
            r->total = fx->value;
            if (fx->external) {
                size_t p;

                /* A link assembles an EXT operand with the provider's offset
                 * already added, so fx->value is the full target. */
                if (!find_provider(lk, res, fx->name, &p)) {
                    fprintf(stderr, "asm816: no provider for external '%s'\n", fx->name);
                    goto out;
                }
                r->seg = (int)p + base;
            }
        }
        segs[k].segnum = (int)k + base;
        segs[k].body = b->bytes;
        segs[k].body_len = b->len;
        segs[k].rel = rels[k];
        segs[k].nrel = b->nfix;
        segs[k].lname = lk->mods[k].lname;
        segs[k].sname = lk->mods[k].sname;
        segs[k].kind = lk->mods[k].kind;
        segs[k].align = lk->mods[k].align;
    }
    err = omf_build(segs, lk->nmods, express, &out, &outlen);
    if (err != NULL) {
        fprintf(stderr, "asm816: %s\n", err);
        goto out;
    }
    f = fopen(lk->segs[0].path, "wb");
    if (f == NULL) {
        perror(lk->segs[0].name);
        goto out;
    }
    if (fwrite(out, 1, outlen, f) != outlen || fclose(f) != 0) {
        fprintf(stderr, "asm816: cannot write %s\n", lk->segs[0].name);
        goto out;
    }
    printf("     => Creating OMF file '%s'\n", lk->segs[0].name);
    ok = true;

out:
    if (rels != NULL) {
        for (k = 0; k < lk->nmods; k++) {
            free(rels[k]);
        }
    }
    free(rels);
    free(segs);
    free(out);
    return ok;
}

static bool write_outputs(const Link *lk, const AsmModule *res)
{
    if (lk->omf) {
        return write_omf_link(lk, res);
    }
    if (lk->nsegs == 0) {
        return write_module_files(lk, res);
    }
    return write_segments(lk, res);
}

/* An OMF link takes its file type from the link file's last TYP, or $B3 when
 * there is none. A flat link takes the last typed module's type, starting from $06. */
static uint8_t link_file_type(const Link *lk, const AsmModule *res)
{
    uint8_t t = TYP_FLAT;
    size_t k;

    if (lk->omf) {
        return lk->typ >= 0 ? (uint8_t)lk->typ : TYP_OMF;
    }
    for (k = 0; k < lk->nmods; k++) {
        if (res[k].bin.typed) {
            t = res[k].bin.file_type;
        }
    }
    return t;
}

static bool write_file_info_for(const Link *lk, const AsmModule *res)
{
    /* With no DSK, the names are the module files. Otherwise they are the DSK names. */
    bool per_module = lk->nsegs == 0;
    size_t count = per_module ? lk->nmods : lk->nsegs;
    const char **names;
    char **owned;
    char *info_path;
    size_t i;
    size_t n = 0;
    bool ok = false;

    if (count == 0) {
        return true;
    }
    names = malloc(count * sizeof *names);
    owned = calloc(count, sizeof *owned);
    info_path = join_dir(lk->dir, "_FileInformation.txt");
    if (names == NULL || owned == NULL || info_path == NULL) {
        goto out;
    }
    for (i = 0; i < count; i++) {
        if (per_module) {
            owned[i] = module_out_name(lk->mods[i].shown);
            if (owned[i] == NULL) {
                goto out;
            }
            if (owned[i][0] == '\0') {
                continue;
            }
            names[n++] = owned[i];
        } else {
            names[n++] = lk->segs[i].name;
        }
    }
    if (n == 0) {
        ok = true;
        goto out;
    }
    ok = file_info_upsert(info_path, names, n, link_file_type(lk, res), lk->aux);
    if (!ok) {
        fprintf(stderr, "asm816: cannot write _FileInformation.txt\n");
    }

out:
    if (owned != NULL) {
        for (i = 0; i < count; i++) {
            free(owned[i]);
        }
    }
    free(owned);
    free(names);
    free(info_path);
    return ok;
}


static void link_free(Link *lk)
{
    size_t i;

    for (i = 0; i < lk->nsegs; i++) {
        free(lk->segs[i].name);
        free(lk->segs[i].path);
    }
    for (i = 0; i < lk->nmods; i++) {
        free(lk->mods[i].shown);
        free(lk->mods[i].path);
        free(lk->mods[i].lname);
        free(lk->mods[i].sname);
    }
    free(lk->segs);
    free(lk->mods);
    free(lk->dir);
}

bool is_link_file(const char *path)
{
    char **lines = NULL;
    size_t n = 0;
    size_t i;
    bool too_long = false;
    bool found = false;

    if (!load_lines(path, &lines, &n, &too_long)) {
        return false;
    }
    for (i = 0; !found && i < n; i++) {
        SrcLine ln;
        const char *err = NULL;

        if (parse_line(lines[i], &ln, &err) && ln.has_opcode &&
            (equal_ci(ln.opcode, "ASM") || equal_ci(ln.opcode, "SNA"))) {
            found = true;
        }
    }
    free_text_lines(lines, n);
    return found;
}

/* Returns the index of the segment that holds module k. */
static size_t segment_of(const Link *lk, size_t k)
{
    size_t s;

    for (s = 0; s < lk->nsegs; s++) {
        if (k >= lk->segs[s].first && k < lk->segs[s].first + lk->segs[s].count) {
            return s;
        }
    }
    return 0;
}

/* Builds "<a>_S<nn>_<b>_Output.txt", or "<a>_S<nn>_Output.txt" without b. */
static char *segment_listing_name(const char *a, size_t index, const char *b, bool has_b)
{
    size_t cap = strlen(a) + strlen(b) + 32;
    char *s = malloc(cap);

    if (s != NULL) {
        if (has_b) {
            snprintf(s, cap, "%s_S%02zu_%s_Output.txt", a, index, b);
        } else {
            snprintf(s, cap, "%s_S%02zu_Output.txt", a, index);
        }
    }
    return s;
}

/* Names module k's listing the way merlin32 does. A single module has no S
 * number. With no DSK the name is the module's SNA name, or its file name
 * when there is none. With a DSK the name uses that DSK and the module's
 * position in link order. An OMF object name is always blank. An OMF
 * ExpressLoad takes segment 1, so it shifts the modules by one. */
static char *listing_name(const Link *lk, size_t k)
{
    const LinkModule *m = &lk->mods[k];
    bool named = m->sname != NULL && m->sname[0] != '\0';
    char *name = NULL;
    size_t cap;

    if (lk->nsegs == 0) {
        char *base = module_out_name(m->shown);

        if (base == NULL) {
            return NULL;
        }
        if (lk->nmods == 1) {
            cap = strlen(base) + 16;
            name = malloc(cap);
            if (name != NULL) {
                snprintf(name, cap, "%s_Output.txt", base);
            }
        } else {
            name = segment_listing_name(named ? m->sname : base, k + 1, "", false);
        }
        free(base);
        return name;
    }
    if (lk->nmods == 1) {
        return dup_str("_Output.txt");
    }
    if (lk->omf) {
        size_t index = k + 1 + (lk->xpl ? 1 : 0);

        return segment_listing_name(lk->segs[0].name, index, "", true);
    }
    return segment_listing_name(lk->segs[segment_of(lk, k)].name, k + 1,
        named ? m->sname : "", true);
}

static bool write_listings(const Link *lk, const AsmModule *res)
{
    size_t k;

    for (k = 0; k < lk->nmods; k++) {
        char *name = listing_name(lk, k);
        char *path = name != NULL ? join_dir(lk->dir, name) : NULL;
        FILE *f;

        if (name == NULL || path == NULL) {
            free(name);
            free(path);
            fprintf(stderr, "out of memory\n");
            return false;
        }
        f = fopen(path, "w");
        if (f == NULL) {
            perror(name);
            free(name);
            free(path);
            return false;
        }
        listing_write(f, &res[k].lst);
        if (fclose(f) != 0) {
            fprintf(stderr, "asm816: cannot write %s\n", name);
            free(name);
            free(path);
            return false;
        }
        printf("     => Creating Output file '%s'\n", name);
        free(name);
        free(path);
    }
    return true;
}

int link_run(const char *path, const char *macro_dir, bool listing)
{
    Link lk;
    AsmModule *res = NULL;
    AsmName *globals = NULL;
    size_t ng = 0;
    int rc = 1;

    memset(&lk, 0, sizeof lk);
    lk.dir = dir_of(path);
    if (lk.dir == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    lk.listing = listing;
    if (!parse_link(path, &lk) || !check_sources(&lk)) {
        goto out;
    }
    res = calloc(lk.nmods > 0 ? lk.nmods : 1, sizeof *res);
    if (res == NULL) {
        fprintf(stderr, "out of memory\n");
        goto out;
    }
    if (!run_layout(&lk, macro_dir, res, &globals, &ng)) {
        goto out;
    }
    if (!check_export_clashes(&lk, res) || !check_imports(&lk, res, globals, ng)) {
        goto out;
    }
    if (!write_outputs(&lk, res) || !write_file_info_for(&lk, res)) {
        goto out;
    }
    if (listing && !write_listings(&lk, res)) {
        goto out;
    }
    rc = 0;

out:
    if (res != NULL) {
        free_modules(res, lk.nmods);
        free(res);
    }
    free(globals);
    link_free(&lk);
    return rc;
}
