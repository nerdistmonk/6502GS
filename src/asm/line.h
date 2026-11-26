#ifndef ASM816_LINE_H
#define ASM816_LINE_H

#include <stdbool.h>

#include "asm.h"

#define OPCODE_MAX 16

typedef struct {
    char label[ASM_NAME_MAX + 1];
    bool has_label;
    char opcode[OPCODE_MAX];
    bool has_opcode;
    /* Operand with blanks outside quotes removed. */
    char operand[ASM_LINE_MAX];
    /* Operand with blank runs outside quotes collapsed to one space. PMC
     * uses it, since a space ends the macro name. */
    char operand_spaced[ASM_LINE_MAX];
    bool has_operand;
    /* Comment text, verbatim from its marker to the end of the line; points
     * into the parsed text. A whole-line comment starts at column 0. */
    const char *comment;
    bool comment_line;
} SrcLine;

/* Splits one source line into label, opcode, and operand. Text after the
 * operand is a comment. On failure returns false and sets *err. */
bool parse_line(const char *text, SrcLine *out, const char **err);

/* Copies src to dst without blanks outside quotes. dst needs strlen(src) + 1
 * bytes. */
void strip_blanks(const char *src, char *dst);

#endif
