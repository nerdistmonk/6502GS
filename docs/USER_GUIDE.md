# asm816 User Guide

asm816 is a cross-assembler for the Apple II family (6502, 65C02, 65C816)
that reads Merlin 8/16/16+ source. It is a clean-room clone of merlin32 and
aims to produce the same bytes, listings, and error messages.

This guide covers the current assembler (Phases 1-4). OMF object output, the
REL directive, and the linker are not yet implemented. Disk image creation,
the Merlin editor, and Sweet 16 mode are out of scope.

## Building

Linux with CMake and GCC. The build uses `-Wall -Wextra -Wpedantic -Werror`.

```
cmake -S . -B build
cmake --build build -j8
cd build && ctest
```

To also run the merlin32 oracle comparisons, configure with the merlin32
binary and its macro library:

```
cmake -S . -B build -DMERLIN32_BIN=/path/to/merlin32 -DMERLIN32_LIB=/path/to/Library
```

## Command line

```
asm816 [--version] [-m <macro-dir>] [-v[N]] <source.s>
```

| Option | Meaning |
|---|---|
| `--version` | Print the version and exit. |
| `-m <dir>` | Macro library directory, searched by `USE` after the source directory. |
| `-v` | Write a listing to `<stem>_Output.txt`. |
| `-vN` | As `-v`, and append the symbol tables with N entries per row. `-v0` means 6. |

The source file must be the last argument.

## Outputs

Each run writes files into the current directory.

- **Object bytes.** A flat binary named after the source file without its
  extension (`hello.s` gives `hello`). If `SAV` or `DSK` gives a name, that
  name is used instead. The file has no load-address header.
- **`_FileInformation.txt`.** Written on every run. It records the output name
  and file type (from `TYP`) in Merlin's file information format.
- **`<stem>_Output.txt`.** Written only with `-v` or `-vN`. It holds the
  listing, followed by the symbol tables with `-vN`. The stem follows the
  `SAV`/`DSK` name when one is set.

If no `ORG` appears, the program counter starts at `$8000`.

## Listing

The listing matches merlin32's `-v` format. Each row shows the line number,
file and line within that file (for included files), line type, MX state,
relocation column, size, address, object code, and the source text.

Errors print to stderr as `      => [Error] <text> (line N, file 'F').` The
assembler stops reporting after the first error.

## Source format

Merlin's four-column layout: label, opcode, operand, comment.

### Numbers, strings, and expressions

- Decimal, `$` hexadecimal, and `%` binary literals.
- Character constants `'x'` and `"x"`.
- `*` is the current program counter.
- Operators are evaluated left to right: `+ - * /` and `! . &`.
- `#`, `<`, and `>` prefixes select immediate, low byte, and high byte, or force
  long addressing (`>$E10000`).
- `@` is not a Merlin literal and is rejected.

### Addressing and opcodes

- 6502 opcodes and all addressing modes.
- 65C02 opcodes, including `BRA`, `STZ`, `TSB`/`TRB`, `INC A`/`DEC A`,
  `PHX`/`PHY`/`PLX`/`PLY`, `WAI`, and `STP`.
- 65C816 opcodes, long addressing, `[dp]` and `[dp],Y`, stack-relative modes,
  block moves, `BRL`, `PER`, and `REP`/`SEP`.
- `MX` sets the immediate width: `%00` to `%11`, or `0` to `3`. Width follows
  source order, including across branches, as merlin32 does.
- `XC` is accepted as a no-op. 65C02 opcodes are accepted without it.

### Directives

| Group | Directives |
|---|---|
| Origin and storage | `ORG`, `EQU`, `DS`, `DFB`/`DB`, `DW`, `DA`, `ADR`, `ADRL`, `DUM`/`DEND`, `END` |
| Data and text | `HEX`, `ASC`, `DCI`, `STR`, `FLS`, `REV`, `INV`, `KBD` |
| Mode | `MX`, `XC` |
| Output hints | `SAV`, `DSK`, `TYP` |
| Listing | `LST`, `PAG`, `TR`, `EXP` (accepted), `ERR` |
| Control flow | `DO`, `ELSE`, `FIN`, `LUP`, `--^`, `MAC`, `EOM` / `<<<`, `PUT`, `USE` |

`ERR expr` fails the build when the value is nonzero or undefined. It is
checked after both passes.

Not yet implemented: `REL`.

### Conditionals

`DO`, `ELSE`, and `FIN` nest without a depth limit. A bare `IF` value is false,
so `IF 1` and `IF 0` skip the body. `IF 1=1` is true. Comparisons use `<`,
`>`, and `=`. `IF MX>1`, `MX/2`, `MX&1`, and `MX=3` test the MX state.

A label on a `DO`, `IF`, `ELSE`, or `FIN` line is defined at the current PC,
even inside a skipped block.

### Loops

`LUP n` repeats its body `n` times, ending at `--^`. Nested `LUP` is an
error. `@` labels inside a loop body become `ozunid_N` names, as in merlin32.
A `@` in a loop over 26 iterations is an error.

### Variables

`]name` defines an assembler variable with `=`, `EQU`, or a label on the line.
It can be redefined, and it keeps its value across passes. A forward reference
to a variable uses its value from the previous pass.

### Macros

```
NAME    MAC
        LDA ]1
        STA ]2
        <<<
```

- `]0` is the argument count, `]1` to `]8` are the positional arguments.
- Labels inside the body become `ozunid_N` names, numbered per expansion.
- `MAC` bodies end at `EOM` or `<<<`.
- A `:` local name in a macro call's operand is an error, as in merlin32.
- `IF c?]n` compares the first character of argument n with `c`. The test is
  decided at expansion.

### Includes

`PUT` and `USE` include other source files. Both are expanded before macros,
loops, and conditionals.

- `PUT` resolves relative to the directory of the file that contains it.
  Absolute paths open directly, as in merlin32.
- `USE` searches the same place, then the macro library (`-m`). A file found
  in the library keeps only its `MAC`…`EOM` blocks.
- A name with at least 3 characters and no `.` in its last path component gets
  `.s` appended.
- Nested `PUT` and `USE` inside an included file do nothing, as in merlin32.
- A `PUT` inside a skipped `DO` still opens its file and errors if the file is
  missing.

## Differences from merlin32

asm816 matches merlin32 for the behaviors above. Known differences:

- **Wide operands.** An address too wide for its 16-bit form (`STA $E10000`)
  is truncated to its low 16 bits (`8D 00 00`), as merlin32 does. asm816 also
  prints a warning to stderr, which merlin32 does not. Use `STA >$E10000` for
  the 24-bit form. Values above `$FFFFFF` are errors, except in the `JML [abs]`,
  `JMP [abs]`, and `JMP (abs)` forms, which truncate like the other 16-bit forms.
- **Bracket and paren prefixes.** The `<` and `>` prefixes inside `[ ]` and
  `( )` are accepted and rejected as in merlin32. `(>dp),Y`, `(>dp,X)`,
  `(abs,X)` with any prefix, `JML [<abs]`, and `[abs],Y` are errors.
- **Register suffix on `JML`/`JMP` brackets.** merlin32 assembles
  `JML [$1234,X]` as `JML [$1234]` and drops the index. asm816 rejects `,X`,
  `,Y`, and `,S` here so the index is not lost silently.
- **Accept/reject.** Some inputs merlin32 accepts are rejected here, including
  bare `DB` and long labels.
- **Multiple errors.** merlin32 can print several errors in one run. asm816
  prints the first one, so a pair such as an unknown line plus a missing `FIN`
  reports only one.
- **Error text.** Common mistakes use merlin's wording. Others keep asm816's
  wording in merlin's line format.
- **Duplicate labels.** The message names the main source for both files,
  because includes are flattened before assembly.
- **Equate order.** The symbol table sorts all equates by value, with
  definition order on ties. merlin32 orders label-based equates (`X1+k`) by a
  key that is not yet understood.
- **Variables and symbols.** Variables (`]x`) are left out of the symbol
  table. Local labels print with a leading `_`.
- **Forward variable references.** A forward reference to a label-style
  variable is an error in merlin32 and is accepted here.
- **`ORG` inside `DUM`.** The listing shows `$FFFF` for the address, as
  merlin32 does. The assembled PC is unchanged.
- **Known merlin32 bug, not reproduced.** After `DO 0` / `DO 0` / `ELSE`,
  merlin32 sizes the next immediate operand as 8-bit even under `MX %00`.
  asm816 sizes it correctly.
- **Listing controls inside expansions.** `LST`, `PAG`, `TR`, and `EXP` rows
  inside a macro expansion or a `LUP` body are hidden, as in merlin32. Their
  line numbers are still consumed. Outside expansions these rows are shown.
  `PAG` writes no form feeds in either tool.
- **Not yet probed.** Whether `KBD` with a label-less line errors in
  merlin32 (we diagnose it). Absolute `PUT`/`USE` paths now match merlin32.

## Testing

`ctest` runs the test suite. With `-DMERLIN32_BIN`, the oracle scripts in
`tests/oracle/` compare bytes (`run_oracle.sh`), symbol tables
(`run_symbols.sh`), and error messages (`run_errors.sh`) against merlin32.
