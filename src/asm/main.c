#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "fileinfo.h"
#include "link.h"
#include "omf.h"

#define PATH_BUF 256

static void usage(FILE *out)
{
    fprintf(out, "usage: asm816 [--version] [-m <macro-dir>] [-v<N>] <source.s>\n");
}

/* Parses merlin32's -v<N> flag. A bare -v is accepted and writes no table.
 * -v<N> appends the symbol tables with N entries per row. */
static bool parse_table_flag(const char *arg, int *table_cols)
{
    const char *p = arg + 2;
    long n = 0;

    if (arg[0] != '-' || arg[1] != 'v') {
        return false;
    }
    if (*p == '\0') {
        return true;
    }
    for (; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        n = n * 10 + (*p - '0');
        if (n > 255) {
            return false;
        }
    }
    *table_cols = (int)n;
    return true;
}

/* Output names follow merlin32: the source basename without its extension,
 * written to the current directory. */
static bool output_stem(const char *path, char *stem, size_t cap)
{
    const char *base = strrchr(path, '/');
    const char *dot;
    size_t n;

    base = base ? base + 1 : path;
    dot = strrchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n == 0 || n >= cap) {
        return false;
    }
    memcpy(stem, base, n);
    stem[n] = '\0';
    return true;
}

static int write_bytes(const char *name, const uint8_t *data, size_t len)
{
    FILE *out = fopen(name, "wb");

    if (out == NULL) {
        perror(name);
        return 1;
    }
    if ((len > 0 && fwrite(data, 1, len, out) != len) || fclose(out) != 0) {
        fprintf(stderr, "asm816: cannot write %s\n", name);
        return 1;
    }
    return 0;
}

/* Writes a standalone REL module as one OMF segment named by its DSK name. */
static int write_omf(const char *name, const Binary *bin)
{
    OmfReloc *rel = malloc((bin->nfix ? bin->nfix : 1) * sizeof *rel);
    OmfSegment seg;
    uint8_t *out = NULL;
    size_t outlen = 0;
    const char *err;
    size_t i;
    int rc = 1;

    if (rel == NULL) {
        fprintf(stderr, "asm816: out of memory\n");
        return 1;
    }
    for (i = 0; i < bin->nfix; i++) {
        rel[i].offset = bin->fix[i].offset;
        rel[i].size = bin->fix[i].size;
        rel[i].shift = bin->fix[i].shift;
        rel[i].external = false;
        rel[i].seg = 0;
        rel[i].total = bin->fix[i].value;
    }
    seg.segnum = 1;
    seg.body = bin->bytes;
    seg.body_len = bin->len;
    seg.rel = rel;
    seg.nrel = bin->nfix;
    seg.name = name;

    err = omf_build(&seg, 1, &out, &outlen);
    if (err != NULL) {
        fprintf(stderr, "asm816: %s\n", err);
    } else {
        rc = write_bytes(name, out, outlen);
    }
    free(out);
    free(rel);
    return rc;
}

/* Merges the output file's entry into _FileInformation.txt, as link mode
 * does, so other entries already in the file are kept. */
static int write_file_info(const char *name, uint8_t file_type)
{
    if (!file_info_upsert("_FileInformation.txt", &name, 1, file_type)) {
        fprintf(stderr, "asm816: cannot write _FileInformation.txt\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char stem[PATH_BUF];
    const char *src;
    const char *name;
    const char *macro_dir = NULL;
    int table_cols = -1;
    bool verbose = false;
    int i;
    Binary bin;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("asm816 %s\n", ASM816_VERSION);
        return 0;
    }

    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc - 1) {
            macro_dir = argv[++i];
        } else if (!parse_table_flag(argv[i], &table_cols)) {
            usage(stderr);
            return 1;
        } else {
            verbose = true;
        }
    }
    if (argc < 2 || argv[argc - 1][0] == '-') {
        usage(stderr);
        return 1;
    }
    src = argv[argc - 1];

    if (is_link_file(src)) {
        if (verbose) {
            fprintf(stderr, "asm816: -v listings are not supported for link files\n");
            return 1;
        }
        return link_run(src, macro_dir);
    }
    if (!output_stem(src, stem, sizeof stem)) {
        fprintf(stderr, "asm816: cannot derive output name from %s\n", src);
        return 1;
    }
    if (!assemble_source(src, macro_dir, verbose ? stem : NULL, table_cols, &bin)) {
        return 1;
    }

    if (bin.rel) {
        if (bin.out_name[0] == '\0') {
            /* Merlin reports this case, writes no file, and exits 0. */
            fprintf(stderr, "Error : Can't create OMF file ''.\n");
            binary_free(&bin);
            return 0;
        }
        name = bin.out_name;
        if (write_omf(name, &bin) != 0) {
            binary_free(&bin);
            return 1;
        }
    } else {
        name = bin.out_name[0] != '\0' ? bin.out_name : stem;
        if (write_bytes(name, bin.bytes, bin.len) != 0) {
            binary_free(&bin);
            return 1;
        }
    }
    if (write_file_info(name, bin.file_type) != 0) {
        binary_free(&bin);
        return 1;
    }
    binary_free(&bin);
    return 0;
}
