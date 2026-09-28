# Linker source fixtures

Fixtures for `codegen.imports.source`
(`tests/codegen/imports/test_source_linker.cpp`), passed to the test
executable as its positional corpus directory. This file is Step 8.1's fixture
inventory: what each file is for, which `protoc` verdict it carries, and how
the differential lane generates it.

## `protoc`

- The plan-4 differential is arbitrated by **`libprotoc 30.1`**
  (`C:\Base\Tools\BIN\protoc.exe`) - the same compiler `reference/oracle.md`'s
  Appendix C, the Step 7.4 corpus and every fix round since were measured with.
- The parser corpus keeps its own baseline, `libprotoc 3.13.0`
  (`../parser/differential/README.md`); that lane and this one are deliberately
  not merged, and this directory never needs `protoc` at test time.

## How the lane generates the corpus (Step 8.3, ledger `D-22`)

Step 8.3 fixes the first edition's three defects: the output directory is
created before `protoc` is asked for anything, `--include_imports` is never
passed, and every fixture gets its own invocation, so one `.pb` decodes to
exactly one `FileDescriptorProto`. On top of those, **each fixture is compiled
with the directory that owns it as `--proto_path` and is passed by its bare
name**: a family whose imports are spelled for itself (`relative/`) cannot be
resolved from the corpus root, and protoc then exits 1 with `Import ... was not
found` - an exit code a reader could not tell from a measured rejection
(ledger `D-22`). `build/plan4/scripts/matrix.py --oracle` runs exactly this and
records each invocation's owning directory and exit code, so no artifact is
unattributed.

```powershell
New-Item -ItemType Directory -Force build/package-imports/oracle | Out-Null
& C:\Base\Tools\BIN\protoc.exe `
    --proto_path=tests/codegen/imports/data/linker `
    --descriptor_set_out=build/package-imports/oracle/report.pb `
    report.proto
& C:\Base\Tools\BIN\protoc.exe `
    --proto_path=tests/codegen/imports/data/linker/relative `
    --descriptor_set_out=build/package-imports/oracle/relative_r01_nopkg_to_subpkg.pb `
    r01_nopkg_to_subpkg.proto
```

The artifact name is the fixture's corpus-relative path with `/` replaced by
`_` and `.proto` replaced by `.pb`. The artifacts go under
`build/package-imports/`, never into the repository.

## Inventory

`protoc 30.1` is the measured verdict; the linker column is what
`codegen.imports.source` asserts. Every accepting fixture has an artifact and is
compared field by field (absolute `type_name` and kind), so the differential
covers it; a rejecting fixture has none, and that absence is the same verdict
re-derived at run time.

| Fixture | proto root | protoc 30.1 | linker | Notes |
| --- | --- | --- | --- | --- |
| `state.proto` | corpus | accept | accept | Contract Task-2 pair, `package shared; enum State { READY = 7; STOPPED = 9; }` (04-import-linker.md). |
| `report.proto` | corpus | accept | accept | The pair's other half: both fields must link to `.shared.State` with `TYPE_ENUM`; the differential compares them. |
| `dep_outer_q.proto` | corpus | accept | accept | Global message `Q` with its nested `R`; the outward fallback the `h01`/`h02` head pair aims at. |
| `h01_service_head_compound.proto` | corpus | **reject** | reject | A service commits a compound head, so `Q.R` fails the member lookup (fix round r10, `D-21`). |
| `h02_service_head_whole_name.proto` | corpus | accept | accept | The same service never wins a whole name, so `Q` walks outward (fix round r10, `D-21`). |
| `relative/dep_ab.proto` | `relative/` | accept | accept | Dependency: `package a.b; message Type`. |
| `relative/dep_pub.proto` | `relative/` | accept | accept | Dependency: publicly re-exports `dep_ab.proto`. |
| `relative/dep_shared.proto` | `relative/` | accept | accept | Dependency: `package shared; message S`. |
| `relative/r01_nopkg_to_subpkg.proto` | `relative/` | accept | accept | A package head covers a member in a sub-package (the prefix rule, fix round r5, `D-14`). |
| `relative/r03_same_pkg.proto` | `relative/` | accept | accept | The same reference with the package spelled out. |
| `relative/r05_partial_visible.proto` | `relative/` | **reject** | reject | A partially visible deeper name is not defined (protoc wording; our one code for the outcome, E1/A3). |
| `relative/r06_pub_import.proto` | `relative/` | accept | accept | The same accept through a public import. |
| `relative/r08_no_import.proto` | `relative/` | **reject** | reject | The import-free reference is not defined; also the source-location case (`test_source_locations_point_at_the_type_token`). |

## The differential's two markers

The suite degrades honestly on a clean checkout: when the oracle directory (or
a `.pb` in it) is absent it prints a `SKIP:` line naming the missing artifact
and exits successfully, so a machine that has not generated the artifacts yet
is not red. A silent pass is not acceptable either, which is why a run that did
decode the generated sets prints one line `ORACLE: compared <n> fixture(s)`:
`matrix.py --oracle` fails a tree whose verbose log carries a `SKIP:` while the
artifacts were generated, and a tree whose suite never printed the coverage
marker. Both markers are read from the documents that print them (the pack's
Step 8.3 and `prompts/impl-08-fixtures-diff.md`), never copied into the runner.

## Not yet registered

Step 8.1 names further source families - `chain/` (ordinary and public edge
variants), `shadowing/` (the `Choice.Item` fixture plus its nearer non-type and
nearer invisible counter-fixtures), `cross_syntax/`, `map_entry/`, `nested/`,
`service_as_type/`, `visibility_roots/`, `defaults/` and
`unsupported_syntax/`. They are the remaining fixture-inventory duty of Step 8
and are not present yet; the sweep-checklist mapping in
`plan4/steps/05-exit-validation.md` names where each of those rows is covered
until they land. The `relative/` subtree keeps its own inventory
(`relative/README.md`), which this table summarizes.
