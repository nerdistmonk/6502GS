#include "listing.h"

#include <stdlib.h>
#include <string.h>

static char *dup_text(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);

    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

static void free_row(ListRow *r)
{
    free(r->label);
    free(r->opcode);
    free(r->operand);
}

void listing_clear(Listing *l)
{
    size_t i;

    for (i = 0; i < l->nrows; i++) {
        free_row(&l->rows[i]);
    }
    for (i = 0; i < l->nnames; i++) {
        free(l->names[i]);
    }
    l->nrows = 0;
    l->nbytes = 0;
    l->nnames = 0;
    l->oom = false;
}

void listing_free(Listing *l)
{
    listing_clear(l);
    free(l->rows);
    free(l->bytes);
    free(l->names);
    memset(l, 0, sizeof *l);
}

bool listing_set_name(Listing *l, int file, const char *name)
{
    char **grown;
    size_t i;

    if (file < 1) {
        return false;
    }
    if ((size_t)file >= l->nnames) {
        size_t want = (size_t)file + 1;

        grown = realloc(l->names, want * sizeof *grown);
        if (grown == NULL) {
            l->oom = true;
            return false;
        }
        for (i = l->nnames; i < want; i++) {
            grown[i] = NULL;
        }
        l->names = grown;
        l->nnames = want;
    }
    free(l->names[file]);
    l->names[file] = dup_text(name);
    if (l->names[file] == NULL) {
        l->oom = true;
        return false;
    }
    return true;
}

bool listing_put_byte(Listing *l, uint8_t b)
{
    if (l->nbytes == l->capbytes) {
        size_t cap = l->capbytes ? l->capbytes * 2 : 256;
        uint8_t *grown = realloc(l->bytes, cap);

        if (grown == NULL) {
            l->oom = true;
            return false;
        }
        l->bytes = grown;
        l->capbytes = cap;
    }
    l->bytes[l->nbytes++] = b;
    return true;
}

bool listing_add_row(Listing *l, const ListRow *row)
{
    ListRow *slot;

    if (l->nrows == l->caprows) {
        size_t cap = l->caprows ? l->caprows * 2 : 256;
        ListRow *grown = realloc(l->rows, cap * sizeof *grown);

        if (grown == NULL) {
            l->oom = true;
            return false;
        }
        l->rows = grown;
        l->caprows = cap;
    }
    slot = &l->rows[l->nrows];
    *slot = *row;
    slot->label = row->label ? dup_text(row->label) : NULL;
    slot->opcode = row->opcode ? dup_text(row->opcode) : NULL;
    slot->operand = row->operand ? dup_text(row->operand) : NULL;
    if ((row->label && slot->label == NULL) || (row->opcode && slot->opcode == NULL) ||
        (row->operand && slot->operand == NULL)) {
        free_row(slot);
        l->oom = true;
        return false;
    }
    l->nrows++;
    return true;
}

static const char *type_name(ListType t)
{
    static const char *const names[] = {
        "Code", "Data", "Directive", "Dum", "Empty", "Equivalence", "Macro", "Variable",
    };

    return names[t];
}

/* Formats up to four bytes starting at p as "XX XX ...". */
static void hex_chunk(const uint8_t *p, size_t n, char *buf, size_t cap)
{
    size_t i;
    size_t used = 0;

    buf[0] = '\0';
    for (i = 0; i < n && used < cap; i++) {
        used += (size_t)snprintf(buf + used, cap - used, "%s%02X", i ? " " : "", p[i]);
    }
}

static void put_dashes(FILE *out, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        fputc('-', out);
    }
}

static void put_rule(FILE *out, size_t fwidth)
{
    fputs("------+", out);
    put_dashes(out, fwidth);
    fputc('+', out);
    put_dashes(out, 13);
    fputs("+----+", out);
    put_dashes(out, 9);
    fputc('+', out);
    put_dashes(out, 6);
    fputc('+', out);
    put_dashes(out, 23);
    fputc('+', out);
    put_dashes(out, 70);
    fputc('\n', out);
}

static void write_row_chunk(FILE *out, const Listing *l, const ListRow *r, size_t fwidth,
                            int lwidth, int owidth, size_t chunk, size_t size)
{
    const uint8_t *bytes = l->bytes + r->boff;
    size_t done = chunk * 4;
    size_t count = size > done ? size - done : 0;
    char hex[16];
    char obj[40];

    if (count > 4) {
        count = 4;
    }
    hex_chunk(bytes + done, count, hex, sizeof hex);

    if (chunk == 0) {
        const char *name = (r->file > 0 && (size_t)r->file < l->nnames && l->names[r->file])
            ? l->names[r->file] : "";

        fprintf(out, "%5lu |", r->line);
        fprintf(out, "%3d %*s %6d |", r->file, (int)fwidth - 12, name, r->fline);
        fprintf(out, " %-11s |", type_name(r->type));
        fprintf(out, " %-2s |", r->mx);
        fputs("         |", out);
        fprintf(out, " %4lu |", (unsigned long)size);
        if (size > 0 || r->type == LST_DATA) {
            snprintf(obj, sizeof obj, " %02X/%04X : %-11s ", (unsigned)((r->pc >> 16) & 0xFF),
                     (unsigned)(r->pc & 0xFFFF), hex);
        } else {
            snprintf(obj, sizeof obj, " %02X/%04X", (unsigned)((r->pc >> 16) & 0xFF),
                     (unsigned)(r->pc & 0xFFFF));
        }
        fprintf(out, "%-23s|", obj);
        fprintf(out, " %-*s%-*s%-15s\n", lwidth, r->label ? r->label : "",
                owidth, r->opcode ? r->opcode : "", r->operand ? r->operand : "");
    } else {
        fputs("      |", out);
        fprintf(out, "%*s|", (int)fwidth, "");
        fprintf(out, "%13s|", "");
        fprintf(out, "%4s|", "");
        fputs("         |", out);
        fprintf(out, "%6s|", "");
        fprintf(out, "           %-11s |\n", hex);
    }
}

void listing_write(FILE *out, const Listing *l)
{
    size_t fwidth = 4;
    size_t lmax = 10;
    size_t omax = 4;
    size_t i;
    int lwidth;
    int owidth;

    for (i = 1; i < l->nnames; i++) {
        if (l->names[i] != NULL && strlen(l->names[i]) > fwidth) {
            fwidth = strlen(l->names[i]);
        }
    }
    for (i = 0; i < l->nrows; i++) {
        const ListRow *r = &l->rows[i];

        if (r->label != NULL && strlen(r->label) > lmax) {
            lmax = strlen(r->label);
        }
        if (r->opcode != NULL && strlen(r->opcode) > omax) {
            omax = strlen(r->opcode);
        }
    }
    lwidth = (int)lmax + 2;
    owidth = (int)omax + 2;
    fwidth += 12;

    put_rule(out, fwidth);
    fprintf(out, " Line | # File%*sLine | Line Type   | MX |  Reloc  | Size | Address   Object Code |  Source Code\n",
            (int)fwidth - 13 + 1, "");
    put_rule(out, fwidth);
    for (i = 0; i < l->nrows; i++) {
        const ListRow *r = &l->rows[i];
        size_t chunks = r->nbytes > 4 ? (r->nbytes + 3) / 4 : 1;
        size_t c;

        for (c = 0; c < chunks; c++) {
            write_row_chunk(out, l, r, fwidth, lwidth, owidth, c, r->nbytes);
        }
    }
    put_rule(out, fwidth);
}
