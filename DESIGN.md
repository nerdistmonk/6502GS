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
- [ ] `XC`, `MX`, and width tracking across branches
- [ ] Directives: `ADR`, `ADRL`, `REL`, `DUM`/`DEND`
- [x] Oracle tests for 65C816 programs (`m816_opcodes`)

### Phase 3: Control flow and text

- [ ] Conditionals: `IF`, `DO`, `ELSE`, `FIN`, nested
- [ ] Loops: `LUP`, `--^`
- [ ] Variables: `]name` assembler variables
- [ ] Macros: `MAC`, `EOM`, positional `]1`..`]9` arguments
- [ ] `PUT` / `USE` includes with search paths
- [ ] Text directives: `STR`, `FLS`, `REV`, `INV`, `KBD`
- [ ] Listing controls: `LST`, `PAG`, `TR`, `EXP`, `ERR`

### Phase 4: Output and completeness

- [ ] `SAV`, `DSK`, `TYP` output hints
- [ ] Symbol table output
- [ ] Full listing parity for the oracle test suite
- [ ] Error-message parity for common mistakes
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
- How strictly to match merlin32 error text versus our own messages?

## Done criteria

The skeleton binary is named `asm816` as a placeholder; rename it if you prefer.

Phases 1-4 are done when the oracle suite passes for 6502, 65C02, and
65C816 sources, and all tests run under `ctest` on Linux with GCC.
