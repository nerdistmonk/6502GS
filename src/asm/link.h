#ifndef ASM816_LINK_H
#define ASM816_LINK_H

#include <stdbool.h>

/* True when the file has an ASM or SNA line, which makes it a link file. */
bool is_link_file(const char *path);

/* Links the flat binary segments named by a link file. Output files and
 * _FileInformation.txt go beside the link file. Returns the exit status. */
int link_run(const char *path, const char *macro_dir);

#endif
