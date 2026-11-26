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

/* A quote opens only if a matching close follows on the line. */
static char quote_step(const char *p, char quote)
{
    if (quote != 0) {
        return *p == quote ? 0 : quote;
    }
    if ((*p == '"' || *p == '\'') && strchr(p + 1, *p) != NULL) {
        return *p;
    }
    return 0;
}

/* Blank runs outside quotes become one space. A blank run followed by ';' or
 * the end of the line ends the operand, and the rest is comment. */
static bool take_operand(const char **pp, char *dst, size_t cap)
{
    const char *p = *pp;
    size_t n = 0;
    char quote = 0;

    while (*p != '\0') {
        if (quote == 0 && (*p == ' ' || *p == '\t')) {
            const char *next = skip_blanks(p);

            if (*next == '\0' || *next == ';') {
                break;
            }
            p = next;
            if (n + 1 >= cap) {
                return false;
            }
            dst[n++] = ' ';
            continue;
        }
        quote = quote_step(p, quote);
        if (n + 1 >= cap) {
            return false;
        }
        dst[n++] = *p++;
    }
    dst[n] = '\0';
    *pp = p;
    return true;
}

void strip_blanks(const char *src, char *dst)
{
    char quote = 0;
    size_t n = 0;

    for (; *src != '\0'; src++) {
        if (quote == 0 && (*src == ' ' || *src == '\t')) {
            continue;
        }
        quote = quote_step(src, quote);
        dst[n++] = *src;
    }
    dst[n] = '\0';
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
    if (!take_operand(&p, out->operand_spaced, sizeof out->operand_spaced)) {
        *err = "operand too long";
        return false;
    }
    strip_blanks(out->operand_spaced, out->operand);
    out->has_operand = true;
    p = skip_blanks(p);
    note_comment(out, p);
    return true;
}
