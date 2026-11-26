#ifndef ASM816_FILEINFO_H
#define ASM816_FILEINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Formats one merlin32 _FileInformation.txt entry, without a line terminator. */
void file_info_format(char *buf, size_t cap, const char *name, uint8_t file_type, uint16_t aux);

/* Adds or replaces the entries for names in the info file at path. An
 * existing entry keeps its line position, and other lines are kept as they
 * are. A missing file is created. Lines end with a newline unless the file
 * is new and holds a single entry, which matches merlin32. Every entry gets
 * the given AuxType. */
bool file_info_upsert(const char *path, const char *const *names, size_t n, uint8_t file_type, uint16_t aux);

#endif
