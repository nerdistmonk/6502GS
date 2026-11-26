#ifndef ASM816_OMF_H
#define ASM816_OMF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One reference the loader must relocate inside a segment body. */
typedef struct {
    /* Body offset of the field. */
    size_t offset;
    /* Field width in bytes: 1, 2 or 3. */
    int size;
    /* 0 for the whole value, -8 for the high byte (>), -16 for the bank (^). */
    int shift;
    /* True when the target is an external label, whose segment is `seg`. */
    bool external;
    /* External only: the 1-based segment number of the provider, 1 to 255. */
    int seg;
    /* The unshifted target: the segment offset of a local label, or the
     * provider's offset plus the addend for an external label. 0 to $FFFF. */
    int32_t total;
} OmfReloc;

/* One segment to write. The body is copied, so the caller keeps ownership. */
typedef struct {
    /* 1-based SEGNUM. */
    int segnum;
    const uint8_t *body;
    size_t body_len;
    OmfReloc *rel;
    size_t nrel;
    /* LOADNAME: the first 10 characters are written; NULL writes blanks. */
    const char *lname;
    /* SEGNAME: up to 10 characters use LABLEN $0A, longer names use a length
     * prefix; NULL writes blanks. */
    const char *sname;
    /* KIND: the segment type and attributes word. */
    uint16_t kind;
    /* ALIGN: 0 NONE, 1 PAGE, 2 BANK. */
    uint32_t align;
} OmfSegment;

/* Builds an OMF v2.1 file of the given segments, in order. On success
 * returns NULL, sets *out to a malloc'd buffer and *outlen to its size. On
 * failure returns a static message and leaves *out NULL. */
const char *omf_build(const OmfSegment *segs, size_t nseg, uint8_t **out, size_t *outlen);

#endif
