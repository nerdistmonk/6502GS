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

In scope (Phases 1-5):

- Assembler: Merlin source dialect, 6502 / 65C02 / 65C816 instruction sets.
- Output: flat binary, listing file, symbol table, OMF v2.1 object files.
- Merlin directives, conditionals, loops, macros, and variables.
- Linker: flat and OMF link files with EXT/ENT across modules.

Deferred (later phases):

- Disk image creation.

Not in scope:

- Editor, Sourceror, or any interactive Merlin UI.
- Sweet 16 pseudo-code mode.
- Windows and macOS code paths.

## Architecture

```
src/asm/assemble.c  passes, PUT/USE, macros, LUP, conditionals, symbols, emitter
src/asm/line.c      line parser (label, opcode, operand, comment)
src/asm/expr.c      expression evaluator and relocation kinds
src/asm/opcodes.c   opcode tables
src/asm/listing.c   listing rows and byte formatting
src/asm/fileinfo.c  _FileInformation.txt
src/asm/omf.c       OMF v2.1 object encoder
src/asm/link.c      link files (flat and OMF), DSK/ORG/ASM/SNA/TYP directives
src/asm/main.c      command-line front end, link mode, and output file naming
tests/              CTest cases; oracle comparisons in tests/oracle/
```

Pipeline: read source → expand `PUT`/`USE` and macros → tokenize lines →
evaluate conditionals and loops → resolve symbols over two passes → emit
bytes and listing.

## Resume here

- Latest commit on `main`: `aacef6a` (docs for Phase 5 stage 2). Phases 0–5
  (flat and OMF linking, standalone REL) are done.
- Build and test: `cmake -S . -B build && cmake --build build && cd build && ctest`.
  Oracle tests need `-DMERLIN32_BIN=/root/merlin32/Source/merlin32
  -DMERLIN32_LIB=/root/merlin32/Library`.
- Probing merlin behavior: `claude_tools/probe_merlin.sh <case.s>` shows its
  listing and bytes. `claude_tools/probe_both.sh <case.s>` diffs merlin32
  against asm816. `claude_tools/build_asan.sh` runs the ASan/UBSan build and ctest.
- Scratch files in `/tmp` are not part of the repo and can be discarded.

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
- [x] Oracle tests: 6502 programs match merlin32 bytes (`tests/oracle/cases/`)

### Phase 2: 65C02 and 65C816

- [x] 65C02 opcodes and addressing modes (zero-page indirect, `(abs,X)` JMP, `BRA`, `STZ`, `TSB`/`TRB`, `INC A`/`DEC A`, `PHX`/`PHY`/`PLX`/`PLY`, `WAI`/`STP`)
- [x] 65C816 opcodes, long addressing (`>` prefix, `JML`/`JSL`), `[dp]`, `[dp],Y`, stack-relative, block moves, `BRL`/`PER`, `REP`/`SEP` immediate width
- [x] `MX` directive (`%00`–`%11` or `0`–`3`; width follows source order, including across branches, as merlin32 does)
- [x] `XC` accepted as a no-op; merlin32 accepts 65C02 opcodes without it, and `XC OFF` is also accepted
- [x] Directives: `REL` (writes an OMF v2.1 object file; see Phase 5)
- [x] Directives: `ADR`, `ADRL`, `DUM`/`DEND`
- [x] Oracle tests for 65C816 programs (`m816_opcodes`, `m816_mx`, `adr_directives`, `dum_dend`)

### Phase 3: Control flow and text

- [x] Conditionals: `DO`, `ELSE`, `FIN`, nested (no depth limit), `IF` on MX expressions (`MX>1`, `MX/2`, `MX&1`, `MX=3`), and `DO`/`IF` comparisons with `<`, `>`, `=`
  - Divergence from the manual, as merlin32 does: a bare `IF` value is false (`IF 1` and `IF 0` skip the body), while `IF 1=1` is true. A `c?]n` test inside a macro is resolved to a comparison, so it stays true when it matches.
  - `IF c?]n` tests compare the first character of argument n with c, as in merlin32. They are decided at macro expansion.
  - Merlin quirks matched: `ELSE` flips the current flag without checking the enclosing block; skipped lines ignore labels, EQU, and operands; 20+ nesting levels accepted.
  - A label on a `DO`/`IF`/`ELSE`/`FIN` line is defined at the current PC, even inside a skipped block, as merlin32 does.
  - Known merlin bug, not matched: after `DO 0` / `DO 0` / `ELSE`, merlin sizes the next immediate operand as 8-bit even under `MX %00`. Our output is correct. The oracle case avoids immediates there.
- [x] Loops: `LUP`, `--^` (nested LUP is an error; `@` labels become `ozunid_N` as merlin32 does, not the manual's A/B/C countdown; `@` in a LUP over 26 iterations is an error)
- [x] Variables: `]name` assembler variables (`=`, `EQU`, or label on a line; redefinable; persist across passes)
  - Merlin's forward reference to a variable uses its value from the previous pass. A forward reference to a label-style variable is an error in merlin and in asm816 (`LDA #]L`, `JMP ]L`, and `DFB ]L` before the `]L` line). The `LDA`/`JMP` messages match merlin; the `DFB` message differs.
- [x] Macros: `MAC`, `EOM`, `]0` (argument count) and positional `]1`..`]8` arguments; `NAME args` and `PMC NAME`; body labels become `ozunid_N` per expansion, as merlin32 does; `:` local names in a macro expansion's operand are an error, as in merlin32
- [x] `PUT` / `USE` includes (expanded at load time, before macros, LUP, and conditionals)
  - `PUT` resolves relative to the source file's directory. `USE` also falls back to the macro library (`asm816 -m <dir>`). Merlin's default library path is not used.
  - Filename rule: the last path component with no `.` and a whole name of length ≥3 gets `.s` appended; otherwise the name is used as written. Absolute paths open directly. No deduplication.
  - A file found via the macro library keeps only its `MAC`…`EOM` blocks.
  - Nested `PUT`/`USE` inside an included file is a silent no-op, as in merlin32. A bare `PUT`/`USE` is silent.
  - A `PUT` inside a skipped `DO` still opens its file and errors if it is missing.
  - Listing line numbers after include expansion match merlin32 (oracle case `include`).
  - Absolute paths work, with or without `.s`, as in merlin32 (oracle case `put_absolute`). An unterminated `MAC` at end of file is silent and its body is not assembled, as in merlin32.
- [x] Macro terminator: `EOM` or `<<<` (merlin's `Macros.s` uses `<<<`)
- [x] Text directives: `ASC`, `DCI`, `STR`, `FLS`, `REV`, `INV`, `KBD`
  - Merlin differs from the manual, and merlin wins: `DCI` inverts the high bit of the last byte even when a hex suffix follows; `STR`'s length byte counts the hex suffix; `INV` and `FLS` mask the hex suffix too; `FLS` accepts only `$20`-`$5F` string characters; `REV` rejects a hex suffix.
  - `KBD` sets its label to the current PC when no key is read (merlin's behavior with no input). The prompt operand is ignored.
  - `STR` over 255 bytes is diagnosed in both tools, with different wording. The object bytes for 255 bytes match merlin32.
  - A label-less `KBD` emits no bytes and is silent in merlin32. We emit the same bytes and listing, with a stderr warning.
- [x] Listing controls: `LST`, `PAG`, `TR`, `EXP` accepted; `ERR expr` fails the build when the value is nonzero or undefined, checked after both passes as merlin32 does
  - Listing format (`-v`) is matched in Phase 4, including control-row hiding inside expansions.

### Phase 4: Output and completeness

- [x] `SAV`, `DSK`, `TYP` output hints (first name wins; `_FileInformation.txt` written)
- [x] Symbol table output: `-v<N>` appends merlin32's numerical and alphabetical tables to the listing with N entries per row (`-v0` is 6; a bare `-v` writes none)
  - The tables go into `<stem>_Output.txt` after the listing, as merlin does. The oracle compares only the table text.
  - Labels print as `$` plus the low 16 bits at four digits. Names longer than 16 characters overflow their column, as merlin does.
  - Divergence: merlin orders label-based equates (`X1+k`) by an unexplained key. We sort all equates by value, with definition order on ties. Oracle cases use constant equates only.
  - Variables (`]x`) are left out. Local labels print as `_name`.
- [x] Full listing parity for the oracle test suite (`-v` writes `<stem>_Output.txt`, named after the DSK/SAV target)
  - All 29 oracle listings match merlin32 (`run_oracle.sh` compares them).
  - `LST`, `PAG`, `TR`, and `EXP` rows are hidden inside macro expansions and `LUP` bodies, and their line numbers are still consumed. Outside expansions they show, and PUT-file rows show. Merlin32 writes no `PAG` form feeds, so we write none either. Covered by `tests/oracle/cases/listing_expansion.s`.
  - Label numbering is two-phase, as merlin32 does. Phase A: each macro expansion, in source order, takes numbers for its MAC-body labels (nested expansions included). Phase B: after all phase A numbers, LUP `@` labels and macro labels inside LUP bodies are numbered per iteration in body order.
  - The first layout run uses a high phase B base so its names cannot collide with later runs. Symbols persist across runs, so names must stay stable.
  - Divergence: an ORG inside a DUM block lists its address as `$FFFF`, as merlin32 does. The assembled PC is unchanged.
- [x] Error-message parity for common mistakes
  - Errors print to stderr as `      => [Error] <text>`, the same form merlin32 uses on stdout. Assembly stops reporting after the first error, which is the lowest-ranked stage (merlin's ordering, approximated).
  - Mapped to merlin text: unknown line, mode/element decode, undefined bracket operand, bad jump, bad HEX, DS negative and over 64 KB, undefined ORG/DS/DB/ERR, ERR nonzero, MX range, duplicate label, missing and extra FIN, missing main source.
  - Divergence: other errors keep our wording in merlin's line format (`<text> (line N, file 'F').`).
  - Divergence: merlin can print several errors in one run. We print one, so a pair such as an unknown line plus a missing FIN reports only the first.
  - Divergence: duplicate-label messages name the main source for both files, because includes are flattened before assembly.
  - Accept/reject differences are not changed (for example bare `DB` and long labels).
  - Covered by `tests/oracle/error_cases/` (`run_errors.sh`), one error per file.
- [x] Wide operands and indirect prefixes, as merlin32 does
  - Absolute, indexed, and `JMP (abs,X)` operands keep their low 16 bits, and `DA`/`DW` keep their low 16 bits. `>` long forms keep 24 bits. Values above `$FFFFFF` are an error in the absolute, indexed, and `JMP (abs,X)` forms.
  - `JML [abs]`, `JMP [abs]`, and `JMP (abs)` truncate above `$FFFFFF` to 16 bits, with no error.
  - Divergence: merlin32 truncates silently. We keep its bytes and print `[Warning] value ... truncated to 16 bits` to stderr.
  - Prefixes inside `[ ]` and `( )` match merlin32's accept/reject set exactly. Accepted: `[>dp]`, `[<dp]`, `[dp],Y`, `(dp)`, `(<dp),Y`, `(<dp,X)`, `(<dp,S),Y`, `JMP (abs)`, `JML [>abs]`, and `JMP [>abs]`. Rejected: `(>dp),Y`, `(>dp,X)`, `(>dp,S),Y`, a prefix on `(abs,X)`, `JML [<abs]`, `JMP [<abs]`, and `[abs],Y`.
  - Not matched: merlin32 ignores a `,X`, `,Y`, or `,S` suffix on `JML`/`JMP` bracket operands. `JML [$1234,X]` assembles as `DC 34 12`, like `JML [$1234]`. We reject these forms. A non-register suffix such as `,garbage` is an error in merlin32 too.
- [x] Documentation: user guide (`docs/USER_GUIDE.md`) and dialect differences from merlin32

### Phase 5: OMF and linking

- [x] OMF v2.1 object output (standalone `REL` modules; byte-identical to merlin32)
- [x] Linker and link scripts: stage 1 flat binary, stage 2 OMF v2.1 output with EXT/ENT across modules. The last `TYP` line picks flat (`$06`) or OMF (any other value, including none). Link rules and divergences are in `docs/USER_GUIDE.md`.

## References

All paths are relative to the reference library at `/root/iigs-workspace/Reference_Materials/`.

- Merlin 16+ manual: `Reference Docs - Software/Merlin_816_Macro_Assembler_Manual_text.txt`
  - Addressing modes: p.83
  - Forced non-zero-page and 65C02/65816 opcodes: p.85-86
  - Pseudo-ops: p.87. Formatting: p.103. Strings: p.107. Data and storage: p.110
  - Conditionals: p.114-116. Miscellaneous: p.119. Macros: p.127. Linker: p.137-155
- 65C02 and 65C816 opcode reference: WDC datasheets in `Reference Docs - Hardware/`
- Oracle binary: merlin32 v1.1.10, used as a black box only

## Open questions

- Command-line interface: resolved. `asm816` mirrors merlin32's flags (`-m <dir>`, `-v<N>`) and adds `--version`.
- Merlin's `,X`/`,Y`/`,S` suffix on `JML`/`JMP` bracket operands (see Phase 4): resolved, keep rejecting. Matching it would silently drop the index.

Unverified against merlin32 (black-box probes still needed):

- [ ] Flat link: the base of a segment whose first module has no `ORG`. We assume 0.
- [ ] No-DSK link output names for paths with directories (`sub/x.s`) and leading-dot names (`.x`). Current names are a guess.
- [ ] Merlin's error precedence when several link errors occur in one link file.
- [ ] Output type in no-DSK links: the link-wide rule (last `TYP` applies to every output) was probed with two files only.
- [ ] `DSK` after an open segment (`ASM`, no `SNA`, then `DSK`). It is an error, carried over from stage 1 and not re-probed in stage 2.

Deferred:

- [ ] Disk image creation (see Scope).

## Done criteria

The binary is named `asm816`.

Phases 1-5 are done when the oracle suites (assembly, error, symbol, and link)
pass for 6502, 65C02, and 65C816 sources, and all tests run under `ctest` on
Linux with GCC. Status: met (`ctest` 65/65, including all oracle suites).
