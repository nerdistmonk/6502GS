#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "fileinfo.h"

#define FOLDER_ZEROS "000000000000000000000000000000000000"

void file_info_format(char *buf, size_t cap, const char *name, uint8_t file_type, uint16_t aux)
{
    snprintf(buf, cap,
        "%s=Type(%02X),AuxType(%04X),VersionCreate(70),MinVersion(BE),Access(E3),"
        "FolderInfo1(%s),FolderInfo2(%s)",
        name, file_type, aux, FOLDER_ZEROS, FOLDER_ZEROS);
}

/* Reads one line of any length without its newline. Returns NULL at end of
 * file or on allocation failure. */
static char *read_line_any(FILE *f)
{
    size_t cap = 256;
    size_t len = 0;
    char *buf = malloc(cap);

    if (buf == NULL) {
        return NULL;
    }
    for (;;) {
        if (fgets(buf + len, (int)(cap - len), f) == NULL) {
            break;
        }
        len += strlen(buf + len);
        if (len > 0 && buf[len - 1] == '\n') {
            buf[len - 1] = '\0';
            return buf;
        }
        if (cap - len <= 1) {
            char *grown = realloc(buf, cap * 2);

            if (grown == NULL) {
                free(buf);
                return NULL;
            }
            buf = grown;
            cap *= 2;
        }
    }
    if (len == 0) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

static size_t find_name(const char *const *names, size_t n, const char *key, size_t klen)
{
    size_t j;

    for (j = 0; j < n; j++) {
        if (strlen(names[j]) == klen && strncmp(names[j], key, klen) == 0) {
            return j;
        }
    }
    return n;
}

/* Writes one line. merlin32 ends every line with a newline once the file has
 * more than one entry, but a lone entry in a new file has none. */
static bool write_line(FILE *out, const char *text, bool terminate)
{
    return fprintf(out, terminate ? "%s\n" : "%s", text) >= 0;
}

/* A name written more than once in a run keeps only its last position, the
 * same as merlin32. */
static bool is_last_name(const char *const *names, size_t n, size_t i)
{
    return find_name(names + i + 1, n - i - 1, names[i], strlen(names[i])) == n - i - 1;
}

bool file_info_upsert(const char *path, const char *const *names, size_t n, uint8_t file_type, uint16_t aux)
{
    char *tmp_path = malloc(strlen(path) + 5);
    FILE *in = NULL;
    FILE *out = NULL;
    char *line;
    size_t i;
    size_t unique = 0;
    bool terminate;
    bool ok = tmp_path != NULL;

    if (!ok) {
        return false;
    }
    snprintf(tmp_path, strlen(path) + 5, "%s.tmp", path);
    for (i = 0; i < n; i++) {
        unique += is_last_name(names, n, i) ? 1 : 0;
    }
    in = fopen(path, "r");
    out = fopen(tmp_path, "w");
    ok = out != NULL;
    terminate = in != NULL || unique > 1;

    while (ok && in != NULL && (line = read_line_any(in)) != NULL) {
        if (find_name(names, n, line, strcspn(line, "=")) == n) {
            ok = write_line(out, line, terminate);
        }
        free(line);
    }
    for (i = 0; ok && i < n; i++) {
        if (is_last_name(names, n, i)) {
            char entry[ASM_LINE_MAX + 256];

            file_info_format(entry, sizeof entry, names[i], file_type, aux);
            ok = write_line(out, entry, terminate);
        }
    }
    if (in != NULL) {
        fclose(in);
    }
    if (out != NULL && fclose(out) != 0) {
        ok = false;
    }
    if (ok) {
        ok = rename(tmp_path, path) == 0;
    }
    if (!ok) {
        remove(tmp_path);
    }
    free(tmp_path);
    return ok;
}

