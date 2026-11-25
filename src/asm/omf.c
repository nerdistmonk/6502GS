#include <stdlib.h>
#include <string.h>

#include "omf.h"

#define HDR_LEN 64
#define BANK_SIZE 0x10000
#define KIND_CODE 0x1000
#define BODY_MAX 0xFFFF
#define NAME_LEN 10

#define OP_END 0x00
#define OP_LCONST 0xF2
#define OP_CRELOC 0xF5
#define OP_CINTERSEG 0xF6
#define OP_SUPER 0xF7

/* SUPER type 0 and 1 are local 2- and 3-byte fields. Type 2 is a 3-byte
 * external field, and type 13 + s is a 2-byte external field in segment s. */
#define SUPER_LOCAL_LO 0
#define SUPER_LOCAL_HI 1
#define SUPER_EXT_LO 2
#define SUPER_EXT_HI 25
#define SUPER_EXT_SEG_BASE 13
#define SUPER_EXT_SEG_MAX 12

typedef struct {
    uint8_t *p;
    size_t n;
    size_t cap;
    bool oom;
} Buf;

static void buf_put(Buf *b, uint8_t c)
{
    if (b->oom) {
        return;
    }
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        uint8_t *grown = realloc(b->p, cap);

        if (grown == NULL) {
            b->oom = true;
            return;
        }
        b->p = grown;
        b->cap = cap;
    }
    b->p[b->n++] = c;
}

static void buf_le(Buf *b, uint32_t v, int bytes)
{
    int i;

    for (i = 0; i < bytes; i++) {
        buf_put(b, (uint8_t)(v >> (8 * i)));
    }
}

static void buf_bytes(Buf *b, const uint8_t *src, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        buf_put(b, src[i]);
    }
}

/* The SUPER type that encodes r, or -1 when cRELOC or cINTERSEG must be used. */
static int super_type(const OmfReloc *r)
{
    if (r->shift != 0) {
        return -1;
    }
    if (!r->external) {
        if (r->size == 2) {
            return SUPER_LOCAL_LO;
        }
        if (r->size == 3) {
            return SUPER_LOCAL_HI;
        }
        return -1;
    }
    if (r->size == 3) {
        return SUPER_EXT_LO;
    }
    if (r->size == 2 && r->seg <= SUPER_EXT_SEG_MAX) {
        return SUPER_EXT_SEG_BASE + r->seg;
    }
    return -1;
}

/* Bytes a cRELOC or cINTERSEG record adjusts. Merlin32 adjusts only the bank
 * byte of a 2-byte field shifted by -16. */
static int record_count(const OmfReloc *r)
{
    if (r->size == 2 && r->shift == -16) {
        return 1;
    }
    return r->size;
}

static const char *check_reloc(const OmfSegment *seg, const OmfReloc *r)
{
    if (r->size < 1 || r->size > 3 || r->offset + (size_t)r->size > seg->body_len) {
        return "relocation outside the segment body";
    }
    if (r->total < 0 || r->total > BODY_MAX) {
        return "relocated value out of range";
    }
    if (r->external && (r->seg < 1 || r->seg > 255)) {
        return "external segment number out of range";
    }
    return NULL;
}

/* Writes the loader-filled value of an external field. A 3-byte field
 * shifted by 0 holds its segment number in the third byte. */
static void patch_external(uint8_t *field, const OmfReloc *r)
{
    int32_t v = r->total >> -r->shift;
    int i;

    for (i = 0; i < r->size; i++) {
        field[i] = (uint8_t)(v >> (8 * i));
    }
    if (r->size == 3 && r->shift == 0) {
        field[2] = (uint8_t)r->seg;
    }
}

static int cmp_offset(const void *a, const void *b)
{
    size_t x = *(const size_t *)a;
    size_t y = *(const size_t *)b;

    return (x > y) - (x < y);
}

/* Writes the SUPER record of one type. Offsets are grouped by 256-byte page.
 * A skip byte (0x80 | n) advances past n pages, and a count byte (offsets - 1)
 * precedes the low byte of each offset in the page. */
static void emit_super(Buf *rec, const OmfSegment *seg, int type)
{
    size_t n = 0;
    size_t i;
    size_t k;
    size_t *offs;
    size_t cur = 0;
    Buf sub = { 0 };

    for (k = 0; k < seg->nrel; k++) {
        if (super_type(&seg->rel[k]) == type) {
            n++;
        }
    }
    if (n == 0) {
        return;
    }
    offs = malloc(n * sizeof *offs);
    if (offs == NULL) {
        rec->oom = true;
        return;
    }
    for (i = 0, k = 0; k < seg->nrel; k++) {
        if (super_type(&seg->rel[k]) == type) {
            offs[i++] = seg->rel[k].offset;
        }
    }
    qsort(offs, n, sizeof *offs, cmp_offset);

    for (i = 0; i < n;) {
        size_t page = offs[i] >> 8;
        size_t j = i;
        size_t skip;

        while (j < n && (offs[j] >> 8) == page) {
            j++;
        }
        for (skip = page - cur; skip > 0;) {
            size_t step = skip > 127 ? 127 : skip;

            buf_put(&sub, (uint8_t)(0x80 | step));
            skip -= step;
        }
        buf_put(&sub, (uint8_t)(j - i - 1));
        for (k = i; k < j; k++) {
            buf_put(&sub, (uint8_t)offs[k]);
        }
        cur = page + 1;
        i = j;
    }

    buf_put(rec, OP_SUPER);
    buf_le(rec, (uint32_t)(sub.n + 1), 4);
    buf_put(rec, (uint8_t)type);
    buf_bytes(rec, sub.p, sub.n);
    if (sub.oom) {
        rec->oom = true;
    }
    free(sub.p);
    free(offs);
}

static void emit_supers(Buf *rec, const OmfSegment *seg, int lo, int hi)
{
    int t;

    for (t = lo; t <= hi; t++) {
        emit_super(rec, seg, t);
    }
}

static void emit_reloc_record(Buf *rec, const OmfReloc *r)
{
    buf_put(rec, r->external ? OP_CINTERSEG : OP_CRELOC);
    buf_put(rec, (uint8_t)record_count(r));
    buf_put(rec, (uint8_t)r->shift);
    buf_le(rec, (uint32_t)r->offset, 2);
    if (r->external) {
        buf_put(rec, (uint8_t)r->seg);
    }
    buf_le(rec, (uint32_t)r->total, 2);
}

/* Appends one segment: header, LCONST body, relocation records, and END. */
static const char *append_segment(const OmfSegment *seg, Buf *file)
{
    Buf rec = { 0 };
    const char *err = NULL;
    uint8_t name[NAME_LEN];
    size_t body_at;
    size_t i;

    if (seg->body_len > BODY_MAX) {
        return "segment body is larger than 64K";
    }
    for (i = 0; i < seg->nrel && err == NULL; i++) {
        err = check_reloc(seg, &seg->rel[i]);
    }
    if (err != NULL) {
        return err;
    }

    buf_put(&rec, OP_LCONST);
    buf_le(&rec, (uint32_t)seg->body_len, 4);
    body_at = rec.n;
    buf_bytes(&rec, seg->body, seg->body_len);

    emit_supers(&rec, seg, SUPER_LOCAL_LO, SUPER_LOCAL_HI);
    for (i = 0; i < seg->nrel; i++) {
        if (!seg->rel[i].external && super_type(&seg->rel[i]) < 0) {
            emit_reloc_record(&rec, &seg->rel[i]);
        }
    }
    emit_supers(&rec, seg, SUPER_EXT_LO, SUPER_EXT_HI);
    for (i = 0; i < seg->nrel; i++) {
        if (seg->rel[i].external && super_type(&seg->rel[i]) < 0) {
            emit_reloc_record(&rec, &seg->rel[i]);
        }
    }
    buf_put(&rec, OP_END);

    for (i = 0; i < seg->nrel; i++) {
        if (seg->rel[i].external) {
            patch_external(rec.p + body_at + seg->rel[i].offset, &seg->rel[i]);
        }
    }
    if (rec.oom) {
        free(rec.p);
        return "out of memory";
    }

    memset(name, ' ', sizeof name);
    if (seg->name != NULL) {
        size_t len = strlen(seg->name);

        memcpy(name, seg->name, len > NAME_LEN ? NAME_LEN : len);
    }

    buf_le(file, (uint32_t)(HDR_LEN + rec.n), 4);
    buf_le(file, 0, 4);
    buf_le(file, (uint32_t)seg->body_len, 4);
    buf_put(file, 0);
    buf_put(file, 0x0A);
    buf_put(file, 0x04);
    buf_put(file, 0x02);
    buf_le(file, BANK_SIZE, 4);
    buf_le(file, KIND_CODE, 2);
    buf_le(file, 0, 2);
    buf_le(file, 0, 4);
    buf_le(file, 0, 4);
    buf_put(file, 0);
    buf_put(file, 0);
    buf_le(file, (uint32_t)seg->segnum, 2);
    buf_le(file, 0, 4);
    buf_le(file, 0x2C, 2);
    buf_le(file, 0x40, 2);
    buf_bytes(file, name, NAME_LEN);
    buf_bytes(file, name, NAME_LEN);
    buf_bytes(file, rec.p, rec.n);

    free(rec.p);
    return NULL;
}

const char *omf_build(const OmfSegment *segs, size_t nseg, uint8_t **out, size_t *outlen)
{
    Buf file = { 0 };
    const char *err = NULL;
    size_t s;

    *out = NULL;
    *outlen = 0;
    for (s = 0; s < nseg && err == NULL; s++) {
        err = append_segment(&segs[s], &file);
    }
    if (err == NULL && file.oom) {
        err = "out of memory";
    }
    if (err != NULL) {
        free(file.p);
        return err;
    }
    *out = file.p;
    *outlen = file.n;
    return NULL;
}
