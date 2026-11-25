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

- [ ] CMake project skeleton, GCC warnings enabled, CTest wired up
- [ ] Test harness and oracle-comparison script
- [ ] Decide language standard (C vs C++17) — see open questions
- [ ] Collect reference excerpts: Merlin 16+ assembler chapter, 65C02/65C816 opcode tables

### Phase 1: Core 6502 assembler

- [ ] Line parser: label, opcode, operand, comment (Merlin four-column layout)
- [ ] Expression evaluator: `+ - * /`, `< > #`, `$ % @` literals, `'x'` chars
- [ ] 6502 opcode table and all addressing modes
- [ ] Two-pass symbol resolution and forward references
- [ ] Directives: `ORG`, `EQU`, `DS`, `DFB`/`DB`, `DW`, `DA`, `HEX`, `ASC`, `DCI`, `END`
- [ ] Flat binary and listing output
- [ ] Diagnostics with line numbers for syntax and range errors
- [ ] Oracle tests: 6502 programs match merlin32 bytes

### Phase 2: 65C02 and 65C816

- [ ] 65C02 opcodes and addressing modes
- [ ] 65C816 opcodes, long addressing, stack-relative modes
- [ ] `XC`, `MX`, and width tracking across branches
- [ ] Directives: `ADR`, `ADRL`, `REL`, `DUM`/`DEND`
- [ ] Oracle tests for 65C816 programs

### Phase 3: Control flow and text

- [ ] Conditionals: `DO`, `ELSE`, `FIN`, nested
- [ ] Loops: `LUP`, `--^`
- [ ] Variables: `]name` assembler variables
- [ ] Macros: `MAC`, `EOM`, positional `]1`..`]9` arguments
- [ ] `PUT` / `USE` includes with search paths
- [ ] Text directives: `STR`, `FLS`, `REV`, `INV`, `KEY`
- [ ] Listing controls: `LST`, `PAG`, `TR`, `EXP`, `ERR`

### Phase 4: Output and completeness

- [ ] `SAV`, `DSK`, `TYP`, `AUX` output hints
- [ ] Symbol table output
- [ ] Full listing parity for the oracle test suite
- [ ] Error-message parity for common mistakes
- [ ] Documentation: user guide and dialect differences from merlin32

### Phase 5 (later): OMF and linking

- [ ] OMF v2.1 object output
- [ ] Linker and link scripts

## Open questions

- Implementation language: C (matches merlin32 layout) or C++17 (matches 6502GS)?
- Command-line interface: mirror merlin32 flags, or a new CLI?
- How strictly to match merlin32 error text versus our own messages?

## Done criteria

Phases 1-4 are done when the oracle suite passes for 6502, 65C02, and
65C816 sources, and all tests run under `ctest` on Linux with GCC.
