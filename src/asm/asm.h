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
    AM_REL
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
} Binary;

/* Two passes over the source. Diagnostics go to stderr; the listing is
 * written to `listing` in the second pass. On success *out owns its bytes. */
bool assemble_source(const char *path, FILE *listing, Binary *out);
void binary_free(Binary *bin);

#endif
