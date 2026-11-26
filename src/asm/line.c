#include <string.h>

#include "line.h"

static const char *skip_blanks(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static bool take_word(const char **pp, char *dst, size_t cap)
{
    const char *p = *pp;
    size_t n = 0;

    while (*p != '\0' && *p != ' ' && *p != '\t') {
        if (n + 1 >= cap) {
            return false;
        }
        dst[n++] = *p++;
    }
    dst[n] = '\0';
    *pp = p;
    return true;
}

/* An operand runs to the first blank outside quotes; anything after is comment. */
static bool take_operand(const char **pp, char *dst, size_t cap)
{
    const char *p = *pp;
    size_t n = 0;
    char quote = 0;

    while (*p != '\0' && (quote != 0 || (*p != ' ' && *p != '\t'))) {
        if (quote != 0) {
            if (*p == quote) {
                quote = 0;
            }
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        }
        if (n + 1 >= cap) {
            return false;
        }
        dst[n++] = *p++;
    }
    dst[n] = '\0';
    *pp = p;
    return true;
}

static void note_comment(SrcLine *out, const char *p)
{
    if (*p == ';') {
        out->comment = p;
    }
}

bool parse_line(const char *text, SrcLine *out, const char **err)
{
    const char *p = text;

    memset(out, 0, sizeof *out);
    if (*p == '*' || *p == ';') {
        out->comment = text;
        out->comment_line = true;
        return true;
    }
    if (*p != ' ' && *p != '\t' && *p != '\0') {
        if (!take_word(&p, out->label, sizeof out->label)) {
            *err = "label too long";
            return false;
        }
        out->has_label = true;
    }
    p = skip_blanks(p);
    if (*p == '\0' || *p == ';') {
        if (*p == ';' && !out->has_label) {
            out->comment = text;
            out->comment_line = true;
        } else {
            note_comment(out, p);
        }
        return true;
    }
    if (!take_word(&p, out->opcode, sizeof out->opcode)) {
        *err = "opcode too long";
        return false;
    }
    out->has_opcode = true;
    p = skip_blanks(p);
    if (*p == '\0' || *p == ';') {
        note_comment(out, p);
        return true;
    }
    if (!take_operand(&p, out->operand, sizeof out->operand)) {
        *err = "operand too long";
        return false;
    }
    out->has_operand = true;
    p = skip_blanks(p);
    out->trailing = *p != '\0' && *p != ';';
    note_comment(out, p);
    return true;
}
