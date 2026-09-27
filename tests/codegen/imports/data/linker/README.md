# Linker source fixtures

Fixtures for `codegen.imports.source`
(`tests/codegen/imports/test_source_linker.cpp`), passed to the test
executable as its positional data directory.

This placeholder exists so the directory is tracked: Git does not record empty
directories, and a directory-only creation would make the intermediate commits
and the `codegen.imports.source` registration inconsistent on a fresh
checkout. Step 8.1 replaces this file with the fixture inventory.

## Head-filter pair (fix round r10, ledger `D-21`)

| Root | protoc 30.1 | linker |
| --- | --- | --- |
| `h01_service_head_compound.proto` | REJECT `"Q.R" is resolved to "inner.Q.R", which is not defined` | reject, unresolved |
| `h02_service_head_whole_name.proto` | ACCEPT (descriptor-set validator re-read exit 0) | accept |

One dependency, `dep_outer_q.proto` (the global message `Q` with its nested
`R`), supplies the outward fallback both roots aim at: `h01`'s committed
service head must not reach it, `h02`'s whole-name walk must.

Verdicts: `build/plan4/fix-r10/corpus/oracle-corpus-summary.txt` (protoc run
against these committed bytes, output directory created first, one invocation
per root, no `--include_imports`). The round's wider case group - which also
settles the package-head and field-head halves of the same rule - is
`build/plan4/fix-r10/oracle/oracle-summary.txt`.

The pair is deliberately at this level, not in `relative/`: the pack's Step
8.3 command (and so the differential lane in `build/plan4/scripts/matrix.py`)
uses this directory as `--proto_path`, and the `relative/` family spells its
imports for itself as the proto root. protoc therefore generates no artifact
for `relative/*` under the lane - `Could not make proto path relative` - which
the lane records as a generation exit code that a reader could mistake for a
verdict (ledger `D-22`).
