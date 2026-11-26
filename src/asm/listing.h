#ifndef ASM_LISTING_H
#define ASM_LISTING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    LST_CODE,
    LST_COMMENT,
    LST_DATA,
    LST_DIRECTIVE,
    LST_DUM,
    LST_EMPTY,
    LST_EQUIV,
    LST_MACRO,
    LST_VARIABLE
} ListType;

/* One displayed listing row. Label, opcode, operand and comment are NULL when
 * absent. For LST_COMMENT rows, comment holds the whole source line. */
typedef struct {
    unsigned long line;
    int file;
    int fline;
    ListType type;
    char mx[3];
    int32_t pc;
    size_t boff;
    size_t nbytes;
    char *label;
    char *opcode;
    char *operand;
    char *comment;
} ListRow;

typedef struct {
    ListRow *rows;
    size_t nrows;
    size_t caprows;
    uint8_t *bytes;
    size_t nbytes;
    size_t capbytes;
    /* Display name for each file index; names[0] is unused. */
    char **names;
    size_t nnames;
    bool oom;
} Listing;

void listing_clear(Listing *l);
void listing_free(Listing *l);
bool listing_set_name(Listing *l, int file, const char *name);
bool listing_put_byte(Listing *l, uint8_t b);
bool listing_add_row(Listing *l, const ListRow *row);
void listing_write(FILE *out, const Listing *l);

#endif
