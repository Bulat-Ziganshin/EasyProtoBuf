# Codegen test coverage by lane

One row per lane directory under `tests/codegen/`. The row names what the lane
proves, the properties it exercises, and the CRs and exceptions its tests
carry. It is a lane map, not a per-fixture ownership table: a fixture may prove
several properties, and the lane `README.md` plus the test names are the
detailed explanation.

The map was aggregated from the 16 `cases.tsv` files of B13 (145 case rows)
before B13a replaced them; the fixtures themselves did not move or change.

| Lane | What it proves | Properties | CRs / exceptions | Notes |
| --- | --- | --- | --- | --- |
| `cli` | CLI transcript, publication snapshots and path refusals | P1; P4; P5; P8; P9 | CR-14; CR-15; CR-16; CR-32; CR-33; CR-34; CR-37; CR-38; CR-39; CR-40; CR-41; CR-47 | `codegen.imports.cli*`, the mode table, unnamed logical files |
| `custom_types` | `easypb.cpp` template spellings and their refusals | P1; P2; P3 | - | `codegen.custom_types*` |
| `enums` | closed and open enums, implicit defaults | P1; P2; P3 | - | `codegen.enums*` |
| `imports` | descriptor/linker corpus, protoc parity table, output and in-memory halves | P1; P2; P3; P4; P5; P6; P7; P8; P9 | CR-06; CR-16; CR-21; CR-22; CR-24; CR-31; CR-32; CR-40; E4; E8; E9; E10; E12 | the widest lane: `codegen.imports.*` |
| `library` | core API equals the CLI; the P7 thin/full/source triple | P1; P2; P3; P6; P7 | CR-08 | `codegen.library*` |
| `limits` | nesting, dependency and import-chain bounds; the physical-root xfail | P1; P3; P8 | CR-35; CR-36; E7 | `codegen.limits*` |
| `maps` | scalar, enum and message map fields | P1; P2; P3 | - | `codegen.maps*` |
| `names` | C++ scope occupants, escaping and insertion stems | P3 | CR-03; CR-04; CR-09; CR-10; CR-43; CR-44 | `codegen.names*` |
| `nested` | nested and forward declarations | P2; P3 | - | `codegen.nested.runtime` |
| `options` | the known-option table (G2) and the custom-option policy | P1; P3; P4 | E13; E14 | `codegen.options` |
| `packages` | package namespaces, imported enum defaults, keyword packages | P1; P2; P3; P7 | CR-08; CR-09; E5; E11 | `codegen.packages*` |
| `parity` | reserved, extension and MessageSet rules on both doors; the CR-25 source-half exception | P1; P2; P3; P7; P9 | CR-06; CR-07; CR-23; CR-24; CR-25; CR-46; E6 | `codegen.parity*` |
| `parser` | grammar, lexer, modes, print and proto3 optional | P1; P2; P4; P5; P8; P9 | CR-01; CR-05; CR-11; CR-12; CR-13; CR-17; CR-18; CR-19; CR-20; CR-26; CR-27; CR-28; CR-29; CR-30; CR-42; CR-48 | `parser.unit`, `codegen.modes` |
| `recursive` | self-recursive repeated and map members | P2; P3 | - | `codegen.recursive.*` |
| `schema` | parser-independent semantics and storage | P1; P2; P9 | CR-01; CR-02; CR-06; CR-07; CR-17; CR-21; CR-23; CR-42; CR-45; CR-46 | `codegen.schema.*` |
| `validity` | defaults, literals and the grammar table, plus pre-existing W families | P1; P2; P3; P4 | CR-01; CR-02; CR-05; CR-11; CR-12; CR-13; CR-17; CR-18; CR-19; CR-20; CR-26; CR-27; CR-28; CR-29; CR-30; E1; E2; E3; E4 | `codegen.validity` |

## Rules

- The 16 lane names are fixed: a new non-lane directory (for example
  `tests/codegen/harness/`) is ignored by the validator and gets no row.
- Every `P1`–`P9` and every `E1`–`E14` appears in the table. Every
  `CR-01`–`CR-48` appears in the table or in a test source under
  `tests/codegen/`.
- A row lists the IDs the lane's tests carry, not every property the code
  touches; the property catalogue owns the definitions.

## The gate

```powershell
python tests/codegen/check_coverage.py
```

The validator checks the lane set, the one-row-per-lane rule, the property and
exception list, the CR set and the absence of the retired per-fixture
manifest. It prints `lane: properties; CR count` for each lane and exits
nonzero with the missing IDs listed.
