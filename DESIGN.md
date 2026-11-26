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

- Latest commit on `main`: `e58c3ee` (`PUT`/`USE` case-insensitive lookup). Uncommitted:
  REL listing named `_Output.txt` when no `SAV`/`DSK` (group d now done). Groups
  (a) to (d) are committed or pending commit; (e) link directives is next and needs
  the XPL/AUX design. Phases 0–5 (flat and OMF linking, standalone REL) are
  done. `ctest` passes 86/86 on GCC Debug with oracle paths.
- Next step: no open link questions remain. Pick the next item from Open
  questions or Deferred (disk image creation). Ask the user before any behavior
  change.
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
- [x] Expression evaluator: left-to-right `+ - * /` and `! . &` at top level, `{ }` RPN precedence, comparisons `< > = #`, `$ %` and decimal literals, `'x'`/`"x"` chars, `*`, `#`/`<`/`>` operand prefixes (`@` is not a Merlin literal; merlin32 rejects it)
  - `$-N` is atoi-style: leading decimal digits of the hex run, negated (`$-10` = -10, `$-9A` = -9, `$-A` = 0).
  - Merlin quirk, reproduced: a `$` literal whose value is exactly -1 is an error in every context (`DB $-1`, `LDA #$-1`, `DS $-1`, `ORG $-1`). `$-2+1` and decimal `-1` are fine. Recorded in `tests/oracle/error_cases/neg_hex_one.s` and `cases/neg_hex.s`.
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
  - `--verbose[N]` (or `--verbosN`, count read from offset 8) is the same as `-vN`; an exact `--verbos` writes no table. `-s[N]` prints the tables to stdout (needs a listing). A bare `-s` takes the most recent `-v<N>` count before it (default 6). `-h`/`--help` print usage. Probed against merlin32 over 22 flag combinations (`tests/oracle/run_symbols.sh`).
  - Open: `-vX` (non-digit suffix) is rejected with usage; merlin32's behavior for it is not probed.
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

## Spec audit

Audit of phases 1–3 against `merlin32_clean_room_behavioral_specification.md`
(black-box, checked against merlin32 byte for byte and in the listing).
Fixed: `{ }` grouping and comparison operators (§9.4); `$-N` (§9.3); `##val`;
`!`/`|` force-absolute prefix; `DS \` at a page boundary (emits 256 bytes, not
1; `DS \,fill` form); EQU listing display `{text}` with nested names expanded;
CLI `-s`, `-h`/`--help`, `--verbose`. Confirmed matches, no change needed: DCI,
SEC+XCE MX, `:local` promotion, IF string compare, `IF MX`, macro opcode
shadowing, left-to-right outside braces, STR over 255 rejected, nested PUT
ignored, `]9` error, `STX $1234,Y` error.

Audit of §2, §3.2, §5–§8, §11–§13 (black-box, runtime-probed against merlin32).
Group (b), macros, fixed (byte-identical to merlin32, oracle cases
`macro_stacked.s`, `macro_explicit_call.s`, `macro_duplicate.s`,
`macro_eom_label.s`): stacked `MAC` headers (body lines go to all open macros
until `EOM`); `PMC` and `>>>` split `name<delim>args` at `.`, `,`, `/`, `-`,
`(` (a space gives no args); duplicate names warn and keep the last definition;
`EOM label` emits an empty row named `ozunid_N` per expansion.
Error-only forms (both assemblers error, exit 1, wording differs, so not oracle
cases): `PMC`/`>>>` with no delimiter (`MOVE#$10`), and a space after the name
(`PMC MOVE #$10;$20`), which passes no arguments.

Group (c), size limits, fixed where measured (error-only, so probe-checked, not oracle cases).
`DUM` errors when its address reaches `$FFFF` (`DUM Object code size > 64 KB`). A non-DUM
run errors when it reaches `$10000` from its `ORG` (`Object code size > 64 KB`). `DS` over
`$FFFF` errors, with the `DUM ` prefix inside a `DUM` block. Of 36 probed runs, asm816
rejects every run merlin32 rejects.

Known mismatch, not matched (asm816 is stricter): merlin32 accepts 10 probed runs that
cross `$FFFF` and asm816 rejects. Examples: `ORG $0 / DS $FFFF / NOP / NOP`, `ORG $0 /
DS $10000` alone, and `ORG $1000 / DS $FFFF / NOP / NOP`. merlin32 also rejects others
that end at the same address (`ORG $0 / DS $FFFD / NOP` x4, `ORG $FFFD / NOP` x4). No
simple address or total-size rule matches all probes, and the cause was not identified.

Gaps confirmed, not yet fixed:

- Bare `ORG` (assembly) is now implemented to match merlin32. The first `ORG $x` sets a resume location, each non-DUM object byte advances it, and a bare `ORG` moves the PC there, with its listing row at that location. Later `ORG $x` lines do not move it. Inside `DUM` it is a no-op. Probed against merlin32 with 10 shapes (listings match) plus `tests/oracle/cases/org_bare.s`. An earlier note that it "keeps the current location" was too simple. The link-file bare `ORG` is separate and already matches (`link_cases/bare_org`).
- `REL` after code, a label, `DS`, `DFB`, or `LDA`: no error in either assembler (probed). The earlier "merlin32 errors" claim was not reproduced and has been dropped.
- Link files: `KND`, `ALI`, `LNA`, `XPL`, `AUX` are unknown directives in asm816 (merlin32 accepts `KND`/`ALI`, rejects conflicting `LNA`). `BSZ` and `DS` before `ASM` already match.
- Error text differs but exit code matches: `ELSE` without `DO`, LUP count over 26, nested LUP, `]9`, `REV` bad string.

Group (a) fixed (byte-identical to merlin32, listing checked): `DDB` (big-endian),
`CHK` (XOR of preceding bytes), `STRL` (2-byte length, no 255 limit), `PUTBIN`
(HEX rows, read from the main source folder), `DAT` 1–8 (synthetic ASC row;
other operands ignored), and the no-op directives (`ANOP`, `START`, `USING`,
`AST`, `CYC`, `LSTDO`, `TTL`, `SKP`, `PAU`, `SW`, `USR`) emit nothing.
Listing parity, now matched byte for byte: CHK's object code shows `00` as in
merlin32 (the binary keeps the real checksum), and `;` comments are shown in the
source column. A comment-only line (`;`, `*`, or an indented `;` with no label
or opcode) is a `Comment` row, printed as one space plus the raw line. A row
with a comment pads its operand to a width of 15–21: the longest operand among
commented rows, capped at 21. A comment follows the padded operand directly, so
an operand of 21 or more characters has no space before `;`. Uncommented rows
still use the same pad width. Comments inside macro bodies and false
conditionals are covered by `tests/oracle/cases/comments*.s`.
`DAT 1 hi` (text after the selector) is ignored by both, but merlin32 lists the
operand as `1hi`; asm816 lists `1`, and the bytes match. Not in the oracle cases.

Matches confirmed: `DS` fill and negative-value error, `HEX` odd-digit error and comma form, `LUP 0` and `LUP 26`, `END` stops reading, CRLF input, `:local` before global, `DO`/`FIN`, equate chains of 9 and 11, `PUT` without extension, 24-bit `ORG`, `REL` before code, plain macro call, stacked `MAC` with no call.

Not yet probed: §14.2 buffer limits, OMF `_Output.txt` naming, `DAT` time selectors 3, 4, 7, and 8 (clock-dependent, so not in oracle cases), and PUTBIN or DAT inside a macro body at invocation.


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

- Command-line interface: resolved. `asm816` mirrors merlin32's flags (`-m <dir>`, `-v<N>`, `-s<N>`, `--verbose<N>`, `-h`) and adds `--version`.
- Open: is `-m` case-insensitive in merlin32, and what does merlin32 do with `-vX` (non-digit)? Not probed.
- Open: the forced-absolute truncation warning (`!`/`|`, wide operands) goes to stderr; merlin32 prints nothing. Keep or suppress?
- Merlin's `,X`/`,Y`/`,S` suffix on `JML`/`JMP` bracket operands (see Phase 4): resolved, keep rejecting. Matching it would silently drop the index.

Link-rule probes against merlin32 (black-box; all resolved):

- [x] Flat link: segment base. Probed 18 variants. merlin32 uses the segment's last `ORG` as the base for all its modules, including those read before it. The first probe matched our old rule, but the two-ORG probe did not, so we now match merlin (`tests/oracle/link_cases/flat_last_org`). A flat `DSK` with no `ORG` anywhere is an error.
- [x] No-DSK link output names. Probed black-box: merlin32 takes the basename, then strips from the last dot. `sub/x.s` gives `x` in the link directory. A leading-dot name (`.s`) is empty. merlin32 reports "Can't create Object file ''", skips it, and exits 0. `_FileInformation.txt` lists only the files written (`link_cases/no_dsk_path_names`, `link_cases/no_dsk_dot_name`).
- [x] Merlin's error precedence when several link errors occur in one link file. Exit code is 1 in every probe, so only the message differs. Decided: keep asm816's first-error reporting. merlin32 checks some whole-file rules first, and labels a misordered directive "Unknown (or misordered)".
- [x] Link-file labels: merlin32 accepts a label on a link line and ignores it (same bytes, exit 0). asm816 now matches (`link_cases/label_ignored`).
- [x] Bare `ORG` (no operand): merlin32 sets the origin to `$0000`, even after an earlier `ORG`. asm816 now matches (`link_cases/bare_org`).
- [x] Output type in no-DSK links: the link-wide rule (last `TYP` applies to every output) matches merlin32 with three modules across seven `TYP` placements, including last-`TYP`-wins and no-`TYP` (OMF, so "DSK is missing").
- [x] `DSK` after an open segment (`ASM`, no `SNA`, then `DSK`), and after an `ORG` with no `ASM`. merlin32 accepts both and writes every segment. asm816 now matches (`link_cases/dsk_after_open`).
- [x] `PUT`/`USE` file names match case-insensitively when the exact name is missing (merlin32 opens `inc.s` for `PUT INC.S`). The exact name wins. Probed; fixed (`oracle/cases/put_case_insensitive.s`). Not defined: which file is used when several names differ only by case.
- [x] REL with no `SAV`/`DSK` and `-v`: the listing is `_Output.txt` (merlin32 gives the same; was `rr_Output.txt`). Probed; fixed. No oracle case, since the runners need an output file. Checked by byte diff against merlin32.
- [x] `DSK`/`SAV` name suffix. merlin32 drops one trailing `.L` (any case) from an assembly `DSK` or `SAV` name: `ABC.L` gives `ABC`, `AB.L.L` gives `AB.L`, `ABC.S` is kept. Link-file `DSK` keeps `.L` (`DSK OUT.L` gives `OUT.L`). asm816 now matches (`oracle/cases/output_dot_l.s`). Not matched: `DSK .L` alone, which merlin32 writes no file for.

Deferred:

- [ ] Disk image creation (see Scope).

## Done criteria

The binary is named `asm816`.

Phases 1-5 are done when the oracle suites (assembly, error, symbol, and link)
pass for 6502, 65C02, and 65C816 sources, and all tests run under `ctest` on
Linux with GCC. Status: met (`ctest` 76/76, including all oracle suites).
