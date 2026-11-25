#include <stdio.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out, "usage: asm816 [--version] <source.s>\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("asm816 %s\n", ASM816_VERSION);
        return 0;
    }

    if (argc != 2) {
        usage(stderr);
        return 1;
    }

    fprintf(stderr, "asm816: assembly not implemented yet\n");
    return 2;
}
