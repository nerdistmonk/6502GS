# Merlin-compatible Assembler: Design & Plan

An open-source (GPL-2.0), clean-room clone of the Merlin32 cross-assembler for
the Apple II family (6502, 65C02, 65C816). Target platform: Linux. Toolchain:
CMake + GCC.

## Clean-room rules

- Implementation is written from the Merlin 8/16/16+ manuals and public
  documentation in the reference library only.
- merlin32 is used as a black-box oracle: compare its output (object bytes,
  listings, error messages) against ours using test inputs.
- No merlin32 source is read, copied, or paraphrased while implementing.
- Every test case and expected output is recorded here or in `tests/`, with
  its provenance noted.

## Scope

In scope (Phase 1-4):

- Assembler: Merlin source dialect, 6502 / 65C02 / 65C816 instruction sets.
- Output: flat binary, listing file, symbol table.
- Merlin directives, conditionals, loops, macros, and variables.

Deferred (later phases):

- OMF v2.1 object output and a linker.
- Disk image creation.

Not in scope:

- Editor, Sourceror, or any interactive Merlin UI.
- Sweet 16 pseudo-code mode.
- Windows and macOS code paths.

## Architecture

```
src/asm/        lexer, expression evaluator, opcode tables, parser, emitter
src/asm/driver  command-line front end (assemble a file, write outputs)
tests/          unit tests and oracle-comparison cases
```

Pipeline: read source → expand `PUT`/`USE` and macros → tokenize lines →
evaluate conditionals and loops → resolve symbols over two passes → emit
bytes and listing.

## Phases and status

Legend: `[ ]` not started, `[~]` in progress, `[x]` done.

### Phase 0: Foundations

- [x] CMake project skeleton, GCC warnings enabled, CTest wired up
- [x] Test harness: CTest smoke test, plus oracle comparison in `tests/oracle/` (enabled with `-DMERLIN32_BIN`)
- [x] Language: C11 (decided by user)
- [x] Collect reference locations (see References)

### Phase 1: Core 6502 assembler

- [x] Line parser: label, opcode, operand, comment (Merlin four-column layout)
- [x] Expression evaluator: left-to-right `+ - * /` and `! . &`, `$ %` and decimal literals, `'x'`/`"x"` chars, `*`, `#`/`<`/`>` operand prefixes (`@` is not a Merlin literal; merlin32 rejects it)
- [x] 6502 opcode table and all addressing modes
- [x] Two-pass symbol resolution and forward references (layout passes repeat until stable; forward EQUs are zero page, forward labels absolute)
- [x] Directives: `ORG`, `EQU`, `DS`, `DFB`/`DB`, `DW`, `DA`, `HEX`, `ASC`, `DCI`, `END`
- [x] Flat binary and listing output
- [x] Diagnostics with line numbers for syntax and range errors
- [x] Oracle tests: 6502 programs match merlin32 bytes (9 cases)

### Phase 2: 65C02 and 65C816

- [x] 65C02 opcodes and addressing modes (zero-page indirect, `(abs,X)` JMP, `BRA`, `STZ`, `TSB`/`TRB`, `INC A`/`DEC A`, `PHX`/`PHY`/`PLX`/`PLY`, `WAI`/`STP`)
- [x] 65C816 opcodes, long addressing (`>` prefix, `JML`/`JSL`), `[dp]`, `[dp],Y`, stack-relative, block moves, `BRL`/`PER`, `REP`/`SEP` immediate width
- [x] `MX` directive (`%00`–`%11` or `0`–`3`; width follows source order, including across branches, as merlin32 does)
- [x] `XC` accepted as a no-op; merlin32 accepts 65C02 opcodes without it, and `XC OFF` is also accepted
- [ ] Directives: `REL` (deferred to Phase 5: merlin writes an OMF object file, so it needs OMF output)
- [x] Directives: `ADR`, `ADRL`, `DUM`/`DEND`
- [x] Oracle tests for 65C816 programs (`m816_opcodes`, `m816_mx`, `adr_directives`, `dum_dend`)

### Phase 3: Control flow and text

- [x] Conditionals: `DO`, `ELSE`, `FIN`, nested (no depth limit), `IF` on MX expressions (`MX>1`, `MX/2`, `MX&1`, `MX=3`), and `DO`/`IF` comparisons with `<`, `>`, `=`
  - `IF c?]n` tests compare the first character of argument n with c, as in merlin32. They are decided at macro expansion.
  - Merlin quirks matched: `ELSE` flips the current flag without checking the enclosing block; skipped lines ignore labels, EQU, and operands; 20+ nesting levels accepted.
  - A label on a `DO`/`IF`/`ELSE`/`FIN` line is defined at the current PC, even inside a skipped block, as merlin32 does.
  - Known merlin bug, not matched: after `DO 0` / `DO 0` / `ELSE`, merlin sizes the next immediate operand as 8-bit even under `MX %00`. Our output is correct. The oracle case avoids immediates there.
- [x] Loops: `LUP`, `--^` (nested LUP is an error; `@` labels become `ozunid_N` as merlin32 does, not the manual's A/B/C countdown; `@` in a LUP over 26 iterations is an error)
- [x] Variables: `]name` assembler variables (`=`, `EQU`, or label on a line; redefinable; persist across passes)
  - Merlin's forward reference to a variable uses its value from the previous pass. A forward reference to a label-style variable is an error in merlin; we accept it (not yet matched).
- [x] Macros: `MAC`, `EOM`, `]0` (argument count) and positional `]1`..`]8` arguments; `NAME args` and `PMC NAME`; body labels become `ozunid_N` per expansion, as merlin32 does; `:` local names in a macro expansion's operand are an error, as in merlin32
- [x] `PUT` / `USE` includes (expanded at load time, before macros, LUP, and conditionals)
  - `PUT` resolves relative to the source file's directory. `USE` also falls back to the macro library (`asm816 -m <dir>`). Merlin's default library path is not used.
  - Filename rule: no `.` and length ≥3 gets `.s` appended; otherwise the name is used as written. No deduplication.
  - A file found via the macro library keeps only its `MAC`…`EOM` blocks.
  - Nested `PUT`/`USE` inside an included file is a silent no-op, as in merlin32. A bare `PUT`/`USE` is silent.
  - A `PUT` inside a skipped `DO` still opens its file and errors if it is missing.
  - Listing line numbers shift after include expansion (not yet matched to merlin).
  - Not yet probed: absolute paths, and an unterminated `MAC` at end of file.
- [x] Macro terminator: `EOM` or `<<<` (merlin's `Macros.s` uses `<<<`)
- [x] Text directives: `ASC`, `DCI`, `STR`, `FLS`, `REV`, `INV`, `KBD`
  - Merlin differs from the manual, and merlin wins: `DCI` inverts the high bit of the last byte even when a hex suffix follows; `STR`'s length byte counts the hex suffix; `INV` and `FLS` mask the hex suffix too; `FLS` accepts only `$20`-`$5F` string characters; `REV` rejects a hex suffix.
  - `KBD` sets its label to the current PC when no key is read (merlin's behavior with no input). The prompt operand is ignored.
  - Not yet probed: `STR` over 255 bytes (we diagnose it), and whether `KBD` with a label-less line errors in merlin (we diagnose it).
- [x] Listing controls: `LST`, `PAG`, `TR`, `EXP` accepted (no effect on the binary; listing format not matched yet); `ERR expr` fails the build when the value is nonzero or undefined, checked after both passes as merlin32 does
  - Merlin's listing output (`LST OFF` suppression, `PAG` form feeds, `EXP` macro expansion lines) is not yet produced. The oracle compares binaries only.

### Phase 4: Output and completeness

- [x] `SAV`, `DSK`, `TYP` output hints (first name wins; `_FileInformation.txt` written)
- [x] Symbol table output: `-v<N>` appends merlin32's numerical and alphabetical tables to the listing with N entries per row (`-v0` is 6; a bare `-v` writes none)
  - We write the tables into the `.lst` rather than a separate `<stem>_Output.txt` as merlin does. The oracle compares only the table text.
  - Labels print as `$` plus the low 16 bits at four digits. Names longer than 16 characters overflow their column, as merlin does.
  - Divergence: merlin orders label-based equates (`X1+k`) by an unexplained key. We sort all equates by value, with definition order on ties. Oracle cases use constant equates only.
  - Variables (`]x`) are left out. Local labels print as `_name`.
- [ ] Full listing parity for the oracle test suite
- [x] Error-message parity for common mistakes
  - Errors print to stderr as `      => [Error] <text>`, the same form merlin32 uses on stdout. Assembly stops reporting after the first error, which is the lowest-ranked stage (merlin's ordering, approximated).
  - Mapped to merlin text: unknown line, mode/element decode, undefined bracket operand, bad jump, bad HEX, DS negative and over 64 KB, undefined ORG/DS/DB/ERR, ERR nonzero, MX range, duplicate label, missing and extra FIN, missing main source.
  - Divergence: other errors keep our wording in merlin's line format (`<text> (line N, file 'F').`).
  - Divergence: merlin can print several errors in one run. We print one, so a pair such as an unknown line plus a missing FIN reports only the first.
  - Divergence: duplicate-label messages name the main source for both files, because includes are flattened before assembly.
  - Accept/reject differences are not changed (for example bare `DB`, `DW $12345`, and long labels).
  - Covered by `tests/oracle/error_cases/` (`run_errors.sh`), one error per file.
- [ ] Documentation: user guide and dialect differences from merlin32

### Phase 5 (later): OMF and linking

- [ ] OMF v2.1 object output
- [ ] Linker and link scripts

## References

All paths are relative to the reference library at `/root/iigs-refs/`.

- Merlin 16+ manual: `Reference Docs - Software/Merlin_816_Macro_Assembler_Manual_text.txt`
  - Addressing modes: p.83
  - Forced non-zero-page and 65C02/65816 opcodes: p.85-86
  - Pseudo-ops: p.87. Formatting: p.103. Strings: p.107. Data and storage: p.110
  - Conditionals: p.114-116. Miscellaneous: p.119. Macros: p.127. Linker: p.137-155
- 65C02 and 65C816 opcode reference: WDC datasheets in `Reference Docs - Hardware/`
- Oracle binary: merlin32 v1.1.10, used as a black box only

## Open questions

- Command-line interface: mirror merlin32 flags, or a new CLI?

## Done criteria

The skeleton binary is named `asm816` as a placeholder; rename it if you prefer.

Phases 1-4 are done when the oracle suite passes for 6502, 65C02, and
65C816 sources, and all tests run under `ctest` on Linux with GCC.
