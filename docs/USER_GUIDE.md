# asm816 User Guide

asm816 is a cross-assembler for the Apple II family (6502, 65C02, 65C816)
that reads Merlin 8/16/16+ source. It is a clean-room clone of merlin32 and
aims to produce the same bytes, listings, and error messages.

This guide covers the assembler (Phases 1-4), the `REL` directive, OMF v2.1
object output, and linking (Phase 5). Disk image creation is deferred. The Merlin
editor and Sweet 16 mode are out of scope.

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
asm816 [--version] [-h] [-m <macro-dir>] [-v[N]|--verbose[N]] [-s[N]] <source.s>
```

| Option | Meaning |
|---|---|
| `--version` | Print the version and exit. |
| `-h`, `--help` | Print the usage text to stdout and exit. |
| `-m <dir>` | Macro library directory, searched by `USE` after the source directory. |
| `-v` | Write a listing to `<stem>_Output.txt`, with no symbol tables. |
| `-vN` | As `-v`, and append the symbol tables with N entries per row. `-v0` means 6. |
| `--verbose`, `--verbosN` | As `-vN`. Bare `--verbose` uses 6 entries per row. Only the first eight characters are checked, and the count is read from the rest, as in merlin32. |
| `-s`, `-sN` | Print the symbol tables to stdout. Needs a listing (`-v` or `--verbose`). Bare `-s` uses the count from the most recent `-vN` or `--verbosN` before it, or 6 if there is none. `-s0` and non-numeric suffixes mean 6. |

The source file must be the last argument.

## Outputs

Each run writes files into the current directory.

- **Object bytes.** A flat binary named after the source file without its
  extension (`hello.s` gives `hello`). If `SAV` or `DSK` gives a name, that
  name is used instead, minus a trailing `.L` (`DSK ABC.L` gives `ABC`, as in
  merlin32). Link-file `DSK` names keep `.L`. The file has no load-address
  header. A source with
  `REL` writes an OMF v2.1 object file instead, named by `SAV` or `DSK`. Without
  either name it prints an error, writes nothing, and exits 0, as merlin32 does.
- **`_FileInformation.txt`.** Written on every run. It records the output name
  and file type (from `TYP`) in Merlin's file information format. An existing
  file keeps its other entries. The entry for this output replaces any old
  entry for the same name and moves to the end.
- **`<stem>_Output.txt`.** Written only with `-v` or `-vN`. It holds the
  listing, followed by the symbol tables with `-vN`. The stem follows the
  `SAV`/`DSK` name when one is set. A `REL` file with no `SAV`/`DSK` writes
  `_Output.txt`, as merlin32 does.

If no `ORG` appears, the program counter starts at `$8000`.

A bare `ORG` (no operand) resumes at the location it tracks: the first `ORG $x`
sets it, and each object byte after that advances it, so later `ORG $x` lines
do not move it. Inside `DUM`, a bare `ORG` does nothing. This matches merlin32.

## Listing

The listing matches merlin32's `-v` format. Each row shows the line number,
file and line within that file (for included files), line type, MX state,
relocation column, size, address, object code, and the source text. Comments
appear in the source column; a comment-only line is shown as a `Comment` row.

Errors print to stderr as `      => [Error] <text> (line N, file 'F').` The
assembler stops reporting after the first error.

## Source format

Merlin's four-column layout: label, opcode, operand, comment.

### Numbers, strings, and expressions

- Decimal, `$` hexadecimal, and `%` binary literals.
- Character constants `'x'` and `"x"`.
- `*` is the current program counter.
- Top-level operators are evaluated left to right: `+ - * /` and `! . &`.
- Inside `{ }` (and in `EQU` values, which are grouped automatically), operators
  use Merlin's precedence: `& . !` bind tighter than `* /`, which bind tighter
  than `+ -`, which bind tighter than comparisons. Comparisons `< > = #`
  evaluate to 1 or 0.
- `$-N` is a negated decimal number. The leading decimal digits after `$-`
  are negated and the rest of the hex-digit run is ignored, so `$-10` is -10,
  `$-9A` is -9, and `$-A` is 0. Merlin also rejects a `$` literal whose value
  is exactly -1 (`DB $-1`, `LDA #$-1`), which asm816 reproduces. Use `-1` for
  the decimal value.
- `#`, `<`, and `>` prefixes select immediate, low byte, and high byte, or force
  long addressing (`>$E10000`). `!` and `|` before an operand force absolute
  addressing, truncated to 16 bits.
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
| Data and text | `HEX`, `ASC`, `DCI`, `STR`, `STRL`, `FLS`, `REV`, `INV`, `KBD`, `DDB`, `CHK` |
| File data | `PUTBIN`, `DAT` |
| Mode | `MX`, `XC` |
| Output hints | `SAV`, `DSK`, `TYP` |
| Linking | `ENT`, `EXT` (see Linking) |
| Listing | `LST`, `PAG`, `TR`, `EXP` (accepted), `ERR` |
| Control flow | `DO`, `ELSE`, `FIN`, `LUP`, `--^`, `MAC`, `EOM` / `<<<`, `PUT`, `USE` |

`DDB` stores 16-bit values high byte first. `STRL` writes a 2-byte little-endian
length, then the text, with no 255-byte limit. `CHK` writes the XOR of every byte
before it, or 0 at the start of the output. Its listing shows object code `00`,
as merlin32 does.

`PUTBIN file` includes the file's bytes in the output and lists them as `HEX`
rows. The file is read from the folder of the main source file.

`DAT n` adds a row with the date or time as text, where `n` is 1 to 8. Selectors
1 and 5 give `DD-MMM-YY`, 2 and 6 give `MM/DD/YY`, and 3, 4, 7, and 8 add the
time. The date comes from the clock at startup. Other operands are ignored.

`ANOP`, `START`, `USING`, `AST`, `CYC`, `LSTDO`, `TTL`, `SKP`, `PAU`, `SW`, and
`USR` are accepted and emit nothing.

`REL` marks the source as a relocatable module. Labels are relative to `$0000`,
and each address or data field that depends on a label is recorded for the
linker. The output is an OMF v2.1 object file (see Outputs). `REL` with an `ORG`
is accepted. The linker uses the module as described in Linking.

`ENT label` exports a label from the module, so other modules can import it.
`EXT label` declares a label that another module exports. The module can use it
in expressions, and the linker fills in its value. Export and import rules are
in Linking.

`ERR expr` fails the build when the value is nonzero or undefined. It is
checked after both passes.

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
- A `MAC` header inside an open body stacks: every following body line goes to
  all open macros until `EOM`. The header line itself is not added to the
  outer macro, as in merlin32.
- `EOM label` (or `label <<<`) adds the label as a body line, so each
  expansion ends with an empty listing row named `ozunid_N`.
- Explicit calls are `PMC name<delim>args` or `>>>name<delim>args`, with
  `<delim>` one of `.` `,` `/` `-` `(`. The arguments follow the delimiter. A
  space after the name gives no arguments, as in merlin32.
- Defining a macro name again prints a warning and keeps the last definition.
- A `:` local name in a macro call's operand is an error, as in merlin32.
- `IF c?]n` compares the first character of argument n with `c`. The test is
  decided at expansion.

### Includes

`PUT` and `USE` include other source files. Both are expanded before macros,
loops, and conditionals.

- `PUT` resolves relative to the directory of the file that contains it.
  Absolute paths open directly, as in merlin32. If the exact name is missing,
  a file whose name differs only in case is used (`PUT INC.S` finds `inc.s`).
  The exact name wins when both exist. If several names differ only by case,
  which one is used is not defined.
- `USE` searches the same place, then the macro library (`-m`). A file found
  in the library keeps only its `MAC`…`EOM` blocks.
- A name with at least 3 characters and no `.` in its last path component gets
  `.s` appended.
- Nested `PUT` and `USE` inside an included file do nothing, as in merlin32.
- A `PUT` inside a skipped `DO` still opens its file and errors if the file is
  missing.

## Linking

A link file is a text file with `ASM` or `SNA` lines. asm816 detects it by
content, so the same command assembles a link file or a source file:

```
asm816 [-m <macro_dir>] links.lk
```

Link files use merlin32's commands:

- `TYP n` sets the output type. The last `TYP` in the file decides the mode for
  the whole link. `TYP $06` gives flat binaries. Any other value, or no `TYP`,
  gives OMF v2.1 output. With no `TYP` the OMF type is `$B3`.
- `DSK name` starts a new output file.
- `ORG addr` sets the origin. A bare `ORG` sets it to `$0000`. A flat `DSK` needs an
  `ORG` somewhere in its segment. An OMF link must not have one. A label on a link
  line is ignored.
- `ASM file.s` adds a module. The segment's origin is its last `ORG`, so every
  module in the segment starts there, plus the bytes earlier modules placed in
  the same output. This includes modules read before that `ORG`.
- `SNA` ends the segment. `SNA` needs an earlier `ASM` in the file. A `DSK` may
  follow a segment that has no `SNA`. The earlier segment is still written.
- `SNA name` sets the OMF segment name (SEGNAME). Names longer than 10 characters
  are written with a length prefix. Without a name, SEGNAME is blank.
- `LNA name` sets the OMF load name (LOADNAME, first 10 characters). The name is
  the rest of the line, so spaces are kept, and one matching pair of `"` or `'`
  around it is removed. Without a name it is blank.
- `KND n` sets the OMF KIND word for the module's segment. The low byte must be
  `$00`, `$01`, `$02`, `$04`, `$08`, `$10`, or `$12`. A bare `KND` gives `$0000`.
  The default is `$1000`.
- `ALI NONE|PAGE|BANK` sets the OMF alignment. The default is `NONE`.
- `XPL` makes an OMF link with two or more modules start with an ExpressLoad
  segment (`~ExpressLoad`, SEGNUM 1). The modules follow as SEGNUM 2 and up. A
  link with one module gets none, and a flat `TYP $06` link ignores `XPL`.
- `AUX value` sets the AuxType of the output's `_FileInformation.txt` entry. Only
  the low 16 bits are kept, and the last `AUX` wins. `$` reads hex, `%` reads
  binary, a leading `#` is skipped, and anything else reads decimal. Text that
  does not parse gives `0000`.
- `LNA`, `SNA`, `KND`, and `ALI` attach to the last `ASM` and need an `ASM` earlier
  in the file. Repeating `LNA` or `SNA` with the same name (ignoring case) is
  fine. A different name is an error. Repeating `KND` or `ALI` takes the last value.

```
 TYP $06
 DSK out
 ORG $1000
 ASM main.s
 ASM util.s
 SNA
```

Each `DSK` writes one output file named after it and updates
`_FileInformation.txt`. A repeated `DSK` name overwrites the earlier file. Labels
exported with `ENT` can be imported by other modules with `EXT`.

For a flat link, each `DSK` writes a flat binary. Modules assembled before the
first `DSK` produce no output, but their labels can still be imported. A `REL`
module in a flat link is placed at its `ORG` like any other module, so its labels
carry the segment address.

For an OMF link, the one `DSK` names the output file. Each module becomes its
own segment, in link order, with `EXT` and `ENT` resolved at link time. An OMF
link can have at most one `DSK`, and it needs one when it has modules.

A flat link with no `DSK` writes one flat file per module at `$8000`. Each file is
named after the basename of its source, with the text from the last dot removed
(`foo.bar.s` gives `foo.bar`, and `sub/x.s` gives `x`). The file is written to
the link file's directory. If two modules share a name, the later one overwrites
the other. A source whose name starts with a dot (`.s`) has an empty name. asm816
reports an error for it, skips that module, and keeps going, with exit code 0 as
merlin32 does.

Differences from merlin32 in link mode:

- **Progress.** merlin32 prints its link log to stdout. asm816 prints progress
  to stderr only.
- **Link-file warnings.** These shapes assemble to the same bytes as merlin32,
  which accepts them silently. asm816 prints `[Warning] Link file` on stderr:
  an `ORG` after `SNA` with no `DSK`, an `ASM` after `SNA` with no `DSK` or `ORG`,
  an `ORG` at end of file with no `ASM`, and an `ASM` at end of file with no `SNA`.
- **Export warnings.** `ENT` of an undefined label, an `EXT` whose name is also a
  local label, and `ENT` with no label each print a `[Warning]`. The `ENT` with no
  label exports the current PC.
- **Object size.** A module that runs past `$FFFF` is an error, as in merlin32.
  merlin32 also writes `error_output.txt`, which asm816 does not.
- **`-v`.** The symbol-table listing is not available in link mode and is an
  error.
- **Addends.** merlin32 drops a `+n` addend from the target field of OMF
  relocation records. asm816 keeps it.
- **Empty OMF link.** An OMF link with no `DSK` and no `ASM` writes nothing and
  exits 0. merlin32 truncates the link file to zero bytes.
- **REL with ORG.** A standalone `REL` file accepts `ORG`, as merlin32 does.

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
- **64 KB limit.** asm816 errors on every run merlin32 rejects for object code
  past 64 KB and for `DUM` past `$FFFF`. It also rejects about ten runs that
  merlin32 accepts, such as `DS $FFFF` followed by NOPs from `ORG $0`, or
  `DS $10000` alone at `ORG $0`. No simple address rule matches merlin32 on
  these cases, so asm816 errs toward rejecting.
- **Multiple errors.** merlin32 can print several errors in one run. asm816
  prints the first one, so a pair such as an unknown line plus a missing `FIN`
  reports only one.
- **Error text.** Common mistakes use merlin's wording. Others keep asm816's
  wording in merlin's line format.
- **Duplicate labels.** The message names the main source for both files,
  because includes are flattened before assembly.
- **Equate order.** The symbol table sorts all equates by value, with
  definition order on ties. This matches merlin32 for plain equates and for
  equates that use one base label (`X1 = L1+k`). When equates use several
  base labels, merlin32's order does not follow from the values, and asm816
  does not reproduce it. Only the order of those lines differs; the values
  are the same.
- **Variables and symbols.** Variables (`]x`) are left out of the symbol
  table. Local labels print with a leading `_`.
- **Forward variable references.** A forward reference to a label-style
  variable is an error in both tools. The `LDA #]L` and `JMP ]L` messages
  match merlin32. The `DFB ]L` message is worded differently.
- **`ORG` inside `DUM`.** The listing shows `$FFFF` for the address, as
  merlin32 does. The assembled PC is unchanged.
- **Known merlin32 bug, not reproduced.** After `DO 0` / `DO 0` / `ELSE`,
  merlin32 sizes the next immediate operand as 8-bit even under `MX %00`.
  asm816 sizes it correctly.
- **Listing controls inside expansions.** `LST`, `PAG`, `TR`, and `EXP` rows
  inside a macro expansion or a `LUP` body are hidden, as in merlin32. Their
  line numbers are still consumed. Outside expansions these rows are shown.
  `PAG` writes no form feeds in either tool.
- **`KBD` without a label.** merlin32 accepts the line silently and emits no
  bytes. asm816 does the same and prints a warning on stderr. Absolute
  `PUT`/`USE` paths now match merlin32.

## Testing

`ctest` runs the test suite. With `-DMERLIN32_BIN`, the oracle scripts in
`tests/oracle/` compare bytes (`run_oracle.sh`), symbol tables
(`run_symbols.sh`), error messages (`run_errors.sh`), and link output
(`run_link.sh`, one directory per case in `link_cases/`) against merlin32.
