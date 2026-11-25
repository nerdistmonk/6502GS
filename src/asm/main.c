#include <stdio.h>
#include <string.h>

#include "asm.h"

#define PATH_BUF 256

static void usage(FILE *out)
{
    fprintf(out, "usage: asm816 [--version] <source.s>\n");
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

int main(int argc, char **argv)
{
    char stem[PATH_BUF];
    char lst_name[PATH_BUF + 4];
    FILE *listing;
    Binary bin;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("asm816 %s\n", ASM816_VERSION);
        return 0;
    }

    if (argc != 2) {
        usage(stderr);
        return 1;
    }

    if (!output_stem(argv[1], stem, sizeof stem)) {
        fprintf(stderr, "asm816: cannot derive output name from %s\n", argv[1]);
        return 1;
    }
    snprintf(lst_name, sizeof lst_name, "%s.lst", stem);
    listing = fopen(lst_name, "w");
    if (listing == NULL) {
        perror(lst_name);
        return 1;
    }

    if (!assemble_source(argv[1], listing, &bin)) {
        fclose(listing);
        return 1;
    }
    if (fclose(listing) != 0) {
        fprintf(stderr, "asm816: cannot write %s\n", lst_name);
        binary_free(&bin);
        return 1;
    }

    if (write_binary(stem, &bin) != 0) {
        binary_free(&bin);
        return 1;
    }
    binary_free(&bin);
    return 0;
}
