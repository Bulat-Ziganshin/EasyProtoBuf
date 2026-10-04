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
re-derived at run time. The corpus holds **52** `.proto` files and the
differential table has **51** rows, **31** of them accepting - and therefore
compared whenever the artifacts exist - while `unsupported_syntax/editions.proto`
is deliberately outside it (the last section says why). A rejection may be
decided at either stage and the table says which: **load** is the frontend's own refusal (a malformed default, an
illegal `packed` option), **link** is the resolver's, and both are rejections a
rejecting fixture must reproduce.

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

## Step 8.1's families

Each family is a self-contained proto root (the lane's per-fixture rule above),
so a family's imports name its own siblings. The column left out of these tables
is the second root: only the four fixtures that need one carry it, and the test
table records it beside the fixture.

### `chain/` - the a -> b -> c chain in both edge variants

| Fixture | protoc 30.1 | linker | Notes |
| --- | --- | --- | --- |
| `chain/ordinary/a.proto` | **reject** | reject (link) | `c.Leaf` is hidden behind `b`'s ordinary import - R3's hidden dependency. |
| `chain/ordinary/b.proto` | accept | accept | Imports `c` privately; its own reference links. |
| `chain/ordinary/c.proto` | accept | accept | The leaf. |
| `chain/public/a.proto` | accept | accept | The same reference links because `b` re-exports `c` publicly. |
| `chain/public/b.proto` | accept | accept | `import public "c.proto";` |
| `chain/public/c.proto` | accept | accept | Same bytes as the ordinary variant's leaf. |

### `shadowing/` - the `Choice.Item` fixture and its counter-fixtures

| Fixture | protoc 30.1 | linker | Notes |
| --- | --- | --- | --- |
| `shadowing/dep.proto` | accept | accept | The outer `pkg.Choice.Item` every counter-fixture aims at. |
| `shadowing/nearer_message.proto` | **reject** | reject (link) | A nearer empty `Choice` commits the head and fails the member lookup (Appendix C B1/B2). |
| `shadowing/nearer_field.proto` | accept | accept | **B8**: a nearer field named `Choice` is a non-type head and must not shadow. |
| `shadowing/nearer_enum_value.proto` | accept | accept | **B13**: a nearer enum value named `Choice` likewise. |
| `shadowing/vis.proto` | accept | accept | The outer `sub` package the invisible-head fixture resolves to. |
| `shadowing/hidden.proto` | accept | accept | The nearer `pkg.sub` message; never imported by anyone. |
| `shadowing/nearer_invisible.proto` | accept | accept | **B6/B10**: `hidden.proto` is loaded as a **second root**, so the nearer head is indexed and invisible. |

### `cross_syntax/` - Step 6's pair plus C3 and C4

| Fixture | protoc 30.1 | linker | Notes |
| --- | --- | --- | --- |
| `cross_syntax/c2/base2.proto` | accept | accept | The proto2 enum provider (closed enum). |
| `cross_syntax/c2/root.proto` | **reject** | reject (link) | Appendix C C2: `Enum type "base2.State" is not an open enum...`. |
| `cross_syntax/c3/msg2.proto` | accept | accept | A proto2 message carrying its own proto2 enum. |
| `cross_syntax/c3/root.proto` | accept | accept | C3's accepting half: the rule keys on the referenced type's own file. |
| `cross_syntax/c4/base2.proto` | accept | accept | **Zero-first** proto2 enum, per the Step 6 decision table row 6.2, so the map case proves the cross-syntax rule alone. |
| `cross_syntax/c4/root.proto` | **reject** | reject (link) | C4: the map's synthetic entry (`app.U.MEntry`). |

### `map_entry/` - the accepting control and every user reference to an entry

| Fixture | protoc 30.1 | linker | Notes |
| --- | --- | --- | --- |
| `map_entry/control.proto` | accept | accept | The owning map field links; the differential compares its synthetic entry type. |
| `map_entry/dep.proto` | accept | accept | The imported owner A5/A6 name through. |
| `map_entry/own_simple.proto` | **reject** | reject (link) | Appendix C A2. |
| `map_entry/own_compound.proto` | **reject** | reject (link) | A3. |
| `map_entry/other_entry.proto` | **reject** | reject (link) | A4 - another message's entry, same file. |
| `map_entry/imported_absolute.proto` | **reject** | reject (link) | A5. |
| `map_entry/imported_relative.proto` | **reject** | reject (link) | A6 - the guard is not keyed on the name's spelling. |
| `map_entry/repeated_simple.proto` | **reject** | reject (link) | A10 - `repeated` cannot be read as "this is the map field". |
| `map_entry/repeated_absolute.proto` | **reject** | reject (link) | A11 - a different entry than the field's own. |

The descriptor-origin half of A22 stays in Step 5.2/7.3's in-memory tests, as the
pack says, and is not duplicated as a hand-written fixture here.

### `nested/`, `service_as_type/`

| Fixture | protoc 30.1 | linker | Notes |
| --- | --- | --- | --- |
| `nested/dep.proto` | accept | accept | An imported message with a nested message AND a nested enum. |
| `nested/root.proto` | accept | accept | Both nested declarations resolve to their absolute names (`.outer.Outer.Inner`, `.outer.Outer.E`). |
| `service_as_type/root.proto` | **reject** | reject (link) | A retained `service` name is not a type; the plan-4 exit criteria name this case. |

### `visibility_roots/` - sweep row R3's two cases

Both fixtures are loaded with `other.proto` as a **second root**, which is what
makes the declaration indexed but unimported; without it the pair would prove
nothing (the name would simply be absent).

| Fixture | protoc 30.1 | linker | Notes |
| --- | --- | --- | --- |
| `visibility_roots/other.proto` | accept | accept | `pkg.Hidden`; never imported by the two fixtures below. |
| `visibility_roots/same_package.proto` | **reject** | reject (link, `DIAGNOSTIC_INVISIBLE_TYPE`) | Case 1: same package, never imported - the name is indexed, so the outcome is invisibility and not "unresolved" (E1-E3). |
| `visibility_roots/second_root.proto` | **reject** | reject (link, `DIAGNOSTIC_INVISIBLE_TYPE`) | Case 2: a second root grants nothing, absolute spelling. |

### `defaults/` - the rejecting side, and its control

| Fixture | protoc 30.1 | linker | Notes |
| --- | --- | --- | --- |
| `defaults/legal.proto` | accept | accept | An enum default naming a real enumerator and `packed = true` on a repeated primitive. |
| `defaults/enum_default_absent.proto` | **reject** | reject (link) | R13b: the default names an enumerator the enum does not declare. |
| `defaults/message_default.proto` | **reject** | reject (**load**) | R13b: a message field cannot carry a default; the frontend refuses it (`enum default must be an identifier`). |
| `defaults/packed_not_packable.proto` | **reject** | reject (**load**) | R13a: `packed = true` on a `repeated string`; the frontend owns this one (`packed option is valid only on repeated primitive or enum fields`). |

### `unsupported_syntax/` - the one recorded inverse

| Fixture | protoc 30.1 | linker | Notes |
| --- | --- | --- | --- |
| `unsupported_syntax/editions.proto` | **accept** | refuse (**load**) | `edition = "2023";`. This is the ONE corpus fixture deliberately outside the differential table: the relation is an inverse, not a comparison, so the suite asserts the refusal in `test_editions_fixture_is_refused`. |

**Measured correction to the pack's 8.1 wording, resolved 2026-10-04.** Step 8.1
says the editions fixture is one "which the parser must refuse with a message
naming editions (A25)". At the 2026-09-28 registration the tree refused it as a
parse error carrying `DIAGNOSTIC_GENERIC` - `expected a top-level .proto
statement` at the `edition` line - because the parser had no `edition` keyword,
while `A25`'s own code (`DIAGNOSTIC_UNSUPPORTED_SYNTAX`) is the
DESCRIPTOR-origin rule. Step 8 filed that divergence as ledger **`D-44`**, a
pack-text row, because `A25`'s naming belongs to plan 5's parser branch.

Plan 5 implemented that branch, and the fixture's header now carries the
re-measurement: the SOURCE frontend refuses the statement by name -
`editions syntax is not supported: only "proto2" and "proto3" are accepted` -
while the refusal stays a load-stage one (the file is never classified, so its
code stays `DIAGNOSTIC_GENERIC` and the message is what names editions). The
fixture is still deliberately outside the differential table: the relation
remains an inverse, `protoc` 30.1 accepting what this project refuses by
decision.

The `relative/` subtree keeps its own inventory (`relative/README.md`), which the
first table summarizes.
