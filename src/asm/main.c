#include <stdio.h>
#include <string.h>

#include "asm.h"
#include "fileinfo.h"
#include "link.h"

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

static int write_binary(const char *name, const Binary *bin)
{
    FILE *out = fopen(name, "wb");

    if (out == NULL) {
        perror(name);
        return 1;
    }
    if ((bin->len > 0 && fwrite(bin->bytes, 1, bin->len, out) != bin->len) || fclose(out) != 0) {
        fprintf(stderr, "asm816: cannot write %s\n", name);
        return 1;
    }
    return 0;
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

    name = bin.out_name[0] != '\0' ? bin.out_name : stem;

    if (write_binary(name, &bin) != 0) {
        binary_free(&bin);
        return 1;
    }
    if (write_file_info(name, bin.file_type) != 0) {
        binary_free(&bin);
        return 1;
    }
    binary_free(&bin);
    return 0;
}
