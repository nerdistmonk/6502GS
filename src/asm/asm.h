#ifndef ASM816_ASM_H
#define ASM816_ASM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define ASM_LINE_MAX 1024
#define ASM_NAME_MAX 63

typedef struct {
    int32_t value;
    bool defined;
} ExprValue;

typedef struct {
    bool (*lookup)(void *ctx, const char *name, int32_t *value);
    void *ctx;
    int32_t pc;
} ExprEnv;

/* Evaluates an expression left to right (no precedence). On a syntax error
 * returns false and sets *err to a static message. */
bool expr_eval(const char *text, const ExprEnv *env, ExprValue *out, const char **err);

typedef enum {
    AM_IMP,
    AM_ACC,
    AM_IMM,
    AM_ZP,
    AM_ZPX,
    AM_ZPY,
    AM_ABS,
    AM_ABX,
    AM_ABY,
    AM_IND,
    AM_IZX,
    AM_IZY,
    AM_IZP,
    AM_IAX,
    AM_REL,
    AM_ALG,
    AM_ALX,
    AM_IDL,
    AM_IDY,
    AM_IAL,
    AM_SR,
    AM_SRY,
    AM_BLK,
    AM_RL16
} AddrMode;

/* True if the 3-letter mnemonic names any 6502 instruction. */
bool opcode_known(const char *mnem);
bool opcode_lookup(const char *mnem, AddrMode mode, uint8_t *opcode);

/* Object code in source order. Like merlin32, ORG does not pad the output:
 * gaps between origins are simply absent. */
typedef struct {
    uint8_t *bytes;
    size_t len;
    size_t cap;
    /* Output file name from the first SAV or DSK; empty when none. */
    char out_name[ASM_LINE_MAX];
    /* ProDOS file type from the last TYP. */
    uint8_t file_type;
} Binary;

/* Two passes over the source. Diagnostics go to stderr. PUT and USE files are
 * found beside the source, and USE falls back to `macro_dir` when it is not
 * NULL. A table_cols of 0 or more appends the symbol tables to the listing
 * with that many entries per row (0 means 6); a negative value writes none.
 * When listing_stem is not NULL the listing goes to "<name>_Output.txt", where
 * name is the first SAV or DSK name or else listing_stem, as merlin32 does.
 * On success *out owns its bytes. */
bool assemble_source(const char *path, const char *macro_dir, const char *listing_stem, int table_cols, Binary *out);
void binary_free(Binary *bin);

/* A label exported by ENT or imported by EXT. */
typedef struct {
    char name[ASM_NAME_MAX + 1];
    int32_t value;
    /* The defining line of an export, or the EXT line of an import. */
    int line;
} AsmName;

/* One source file of a link. The bytes, exports, and imports are kept even
 * when assembly reports errors, so a link can size its modules before the
 * final pass. The caller releases it with module_free. */
typedef struct {
    Binary bin;
    AsmName *exports;
    size_t nexports;
    AsmName *imports;
    size_t nimports;
} AsmModule;

/* Assembles one link module that starts at start_pc. EXT names take their
 * values from globals, and an EXT name with no global is an error when used.
 * Errors print to stderr only when print_errors is true. */
bool assemble_module(const char *path, const char *macro_dir, int32_t start_pc,
    const AsmName *globals, size_t nglobals, bool print_errors, AsmModule *out);
void module_free(AsmModule *m);

#endif
