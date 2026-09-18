# Source loading and the import graph

This document explains what the loader implements and why it works the way it does. It is written as
prose on purpose: it is meant to be read end to end, not to serve as a specification or a requirement
checklist. It complements [INTERNALS.md](INTERNALS.md), which maps the source layout, and
[parser/README.md](parser/README.md), which owns the parser-level API.

All statement counts and symbol references are given **as of commit `075530e`** ("Codegen: imports
part 3 - source loader and import graph"), which carries the change described here. A few sections
measure the implementation against "the plan": the written requirements for this step, drafted while
the change was planned and not part of this repository.

## Summary

Before this change `codegen` could parse a single input file and record its imports in the descriptor,
but it never opened an imported file: anything not declared in that one file was unresolvable, and
`main.cpp` refused to continue on such a schema. The change adds the other half of the input path — an
ordered list of search roots (`-I`) plus a list of input files becomes a `SchemaSet` holding every
file, transitive imports included, each exactly once, with an already validated import graph: no
missing imports, no cycles, no two logical names for one physical file. The same graph is validated for
descriptor-set input as well, with no filesystem access at all. Types and visibility (`public import`
as re-export) are still not resolved; that is the next step of the plan.

## The problem

A line `import "a/b.proto"` is just a string. It does not say which file to read. That splits into
three distinct duties that are easy to conflate:

1. **Find the file.** Through ordered search roots, with an unambiguous precedence rule. Two roots may
   both contain `a/b.proto`, and choosing between them is part of the contract, not an accident.
2. **Never be wrong about file identity.** One file must not be read twice (a diamond, a repeated
   `-I`, a hardlink under another name), and two different files must not collapse into one.
3. **Explain a refusal.** Name the import, the importing file, the declaration site, and the roots
   that were searched. Without that, a diagnostic is indistinguishable from "something did not
   compile".

Everything else in the change is either groundwork for the following steps or voluntary strictness
(see "Beyond the plan").

## Three roles in four places

The split is not modularity for its own sake. The project has a build profile without the source
parser (descriptor sets only), and a plugin mode is planned; neither should pull in filesystem code.

| Role | Where | Lines | Who links it |
|---|---|---|---|
| Logical name rules | `logical_paths.{hpp,cpp}` | 39 + 138 | every profile, including descriptor-only and plugin |
| Graph ownership | `bind_import_edges` and helpers in `schema.{hpp,cpp}` | +312 in the `.cpp`, +36 in the `.hpp` | every profile |
| Disk adapter | `file_paths.{hpp,cpp}` | 249 + 1362 | full builds only |
| Source loader | `proto_loader.{hpp,cpp}` | 68 + 815 | full builds only |

In CMake terms:

- `easypb_schema` = `schema.cpp` + `schema_semantics.cpp` + `logical_paths.cpp` (the new file);
- `easypb_proto_parser` = the parser, on top of `easypb_schema`;
- `easypb_proto_loader` = `proto_loader.cpp` + `file_paths.cpp`, on top of both, and created only when
  `EASYPB_CODEGEN_WITH_PROTO_PARSER` is on.

Why the name rules are separate rather than private to the loader: a logical name is the shared
contract of three frontends — source input, descriptor-set input, and future plugin input — and it must
be validated identically for all of them. That means the validation has to work without the parser and
without the filesystem, or descriptor-set input could not call it at all.

## The path of one call

Everything starts in `load_source_files(options, roots, result, error, statistics)`
(`proto_loader.cpp:743`): clear the result, normalize the roots, then for every input argument call
`resolve_root` and load recursively, append the loaded file to `targets`, and call `bind_import_edges`
once at the very end. The order is not incidental: until every file is loaded there is no graph to
validate.

### Normalizing the roots

An empty root list means the current working directory, and an empty entry means the same thing. It
looks like a detail, but it is what makes the behavior match what a user expects when a script passes
`-I ""`.

### Turning an argument into a logical name (`resolve_root`, lines 596–743)

An argument is first tried **as a disk path**, and only if no such file exists is it treated as a
logical name. Disk first is deliberate: a spelling that names a real file stays a disk path even when
it would also be a valid logical name.

When the file exists, `map_disk_to_logical` (lines 165–309) finds the first search root that contains
the path lexically, strips the root prefix, and produces a logical name. The name is then
**re-verified**: if that logical name resolves to a different file under the same ordered roots
(compared by native identity), this is reported as a shadowed root instead of being treated as a
rename. The point of the re-check is simple: the worst outcome here should be an extra error, never a
silent compilation of the wrong file.

When the argument is not a disk path, `resolve_logical` (lines 87–118) walks the roots in precedence
order and the first one that has the file wins. **An operational failure stops the search** and does
not advance to the next root; only absence may advance it. Otherwise a permissions mistake would be
masked by a different file found under a later root.

The third case is a file that exists nowhere under the roots but is spelled as a fully qualified path
(drive-absolute or UNC on Windows, leading `/` elsewhere). That is the isolated mode: the file is used
under its original spelling, but only if it declares no imports — otherwise there are no roots to
resolve its neighbors from, which is an immediate error suggesting an explicit `-I`. See "Beyond the
plan": this convenience is not required by the plan.

### Loading one file: four checks in order (`load_recursive`, lines 478–594)

1. **The logical name is already present and maps to the same file** — return silently, so a repeated
   command-line root is not an error.
2. **The same physical file appears under a different logical name** — an error naming both. The cause
   does not matter: symlink, hardlink, `./x` versus `x`, or a case difference on Windows.
3. **The slot is reserved in the `SchemaSet` before imports are followed.** This is what makes both a
   diamond and a cycle safe: no file is parsed twice and no recursion can be infinite. The slot is
   empty at this point; parsing fills it.
4. **Reading and parsing happen in a separate helper** (`read_and_parse_source`, lines 322–379). The
   file is read into a local buffer and parsed deferred (`defer_type_resolution = true`); the slot gets
   `file.name` = the logical name and `physical_name` = the disk path; the buffer dies before
   dependencies are followed.

Check 4 answers "why deferred, and why two names". The source must not outlive parsing, because
otherwise it is retained for the whole transitive walk while the import depth is not known in advance.
At the same time `physical_name` is needed afterwards for diagnostics and alias checks, and `file.name`
must hold the logical path: generated include names come from it, and it is the key that matches files
against each other.

### Imports (`load_dependencies`, lines 381–477)

The walk follows the descriptor's `dependency` list, which is authoritative. The modifier comes from
`weak_dependency` / `public_dependency` by index, and the parser's `imports` vector is used **only** as
a source of positions for messages. Every import is loaded, including ones whose types nobody
references, and a `weak` import has the same existence requirement as a normal one in this complete
mode.

### Final graph validation (`bind_import_edges`, `schema.cpp:275`)

Edges are built with a stable pointer to the target `SchemaFile` inside the `SchemaSet` (files live on
the heap, so addresses do not move as the vector grows), plus the dependency index — which keeps an
edge identifiable even when the target is unknown — and the modifier. Cycle detection then runs as an
iterative depth-first search with an explicit stack, so the message can report the chain from the root
to the declaration that closes the cycle instead of merely stating that a cycle exists.

## Five invariants

- **Descriptors outrank `imports`.** The `dependency` / `public_dependency` / `weak_dependency` lists
  are the source of truth; the `imports` vector only supplies coordinates for diagnostics. This is why
  the binder also works on descriptor-set input, where `imports` does not exist at all.
- **File identity is native, not textual.** `st_dev`/`st_ino` on POSIX, volume plus file index on
  Windows (`FileIdentity` in `file_paths.hpp`). Case sensitivity is a property of the filesystem, not
  of a file, so lowercasing a path string to "compare paths" would make assumptions the platform does
  not promise.
- **Caching happens before recursion, and "this is a cycle" is decided in the binder.** The loader
  prevents infinite work; the common pass is what reports a cycle as an error. The separation is
  deliberate: cycles must be found identically for source input and for descriptor sets.
- **A failure leaves no partial result.** Any error clears the `SchemaSet` and all edges
  (`clear_all_edges`), and an empty input yields an empty result. A failure must not look like a
  successful compilation of fewer files.
- **Order is deterministic.** `targets` follow command-line order and edges follow declaration order.
  That is what makes output reproducible, and it is also why tests can compare exact messages.

## What counts as an error

**Within one file:** a repeated name in the dependency list; an invalid import spelling; a
`public_dependency` / `weak_dependency` index that is negative, out of range, repeated, or names a
dependency already marked by the other list.

**In the graph:** an import that is absent on disk, even when no type references it; a cycle; one
physical file under two logical names; a shadowed root; an operational failure while reading a file or
a directory.

Three diagnostic codes were added next to the existing ones (`schema.hpp:38–42`):
`DIAGNOSTIC_INVALID_IMPORT`, `DIAGNOSTIC_MISSING_IMPORT`, `DIAGNOSTIC_IMPORT_CYCLE`. No existing code
was changed, and a test asserts that the codes stay distinct.

Messages use two chain spellings on purpose: the binder prints its discovery path unquoted
(`import cycle: a.proto -> b.proto -> a.proto`), while the loader appends the selected root in quoted
form (`; import chain "root.proto" -> "middle.proto"`). These are two different facts — the graph path
and the discovery path — not an inconsistency.

## What this change does not do

- It does not resolve types or implement visibility (`public import` as re-export); that is the linker
  step.
- It does not register command-line flags or replace the generation pipeline.
- It does not write files or create directories.
- It does not handle process argument-encoding at process entry.

This is verifiable in one command: `grep -c "load_source_files" codegen/main.cpp codegen/codegen.cpp`
returns `0`. The loader is a library with no consumer yet, which is exactly why the change can feel
large relative to its current effect. The only `main.cpp` edits are a guard against an embedded NUL in
a file name, a scoped switch of console output to UTF-8, and a `catch (...)` so an unknown exception
does not kill the process silently.

## Beyond the plan

This section covers work the plan text does not require but the change carries.

**Windows case and Unicode folding machinery.** Per-directory case-sensitivity queries
(`FileCaseSensitiveInfo`), entry-by-entry confirmation of a folded prefix match, lexical equivalence of
isolated-root spellings, and junction/reparse handling. For comparison, the reference implementation of
the same mechanism in protobuf (`DiskSourceTree`, `compiler/importer.cc`) does a byte-wise prefix
comparison plus a component-boundary check, with no folding at all, even on Windows.

**The isolated mode as a whole** (`resolve_root`, the `is_fully_qualified_disk_path` branch): the
convenience of an absolute path to a file without imports and without `-I`. The reference has no such
mode: the input file must live under some `--proto_path`.

**The infrastructure around it**: a separate `EASYPB_EXTENDED_TESTS` profile (off by default), a new
`extended-tests` CI job on three operating systems, and a Windows manifest declaring
`<activeCodePage>UTF-8</activeCodePage>` attached through a generated `.rc` file, so narrow paths are
read as UTF-8 regardless of the system code page.

Options: keep it but land it as its own commit with a stated rationale, or drop it. Dropping is cheap
right now precisely because the loader has no consumer: removing the case/fold machinery and the
isolated mode takes out about a third of the loader and a whole class of "did we guess the case right"
questions from diagnostics. That is an assessment, not a requirement; the decision belongs to the plan
owner.

## Reading order

1. `logical_paths.cpp` (138 lines) — self-contained, and the right place to start.
2. `bind_import_edges` in `schema.cpp:275` — the graph core, about 230 lines.
3. `load_source_files` in `proto_loader.cpp:743` — about 70 lines, the whole outer contour.
4. `resolve_root` and `map_disk_to_logical` — only after that; the root logic is the least pleasant
   part and the most rewarding to read last.
5. `strip_search_root_prefix`, `lexical_absolute`, `normalize_separators`, `directory_case_sensitive`
   and the rest of `file_paths.cpp` — read on demand, not front to back.

## Appendix A: Glossary

**Logical name** — the relative path with `/` separators stored in `FileDescriptorProto.name` and in
the dependency lists, and reused for generated include names. It is a strict platform-independent
UTF-8 namespace: absolute spellings, backslashes, empty components, `.`/`..`, quotes, control bytes,
and malformed UTF-8 are rejected.

**Native identity** — the value the operating system returns for "is this the same file?": device and
inode on POSIX, volume and file index on Windows. It is the only reliable way to tell one file under
two names from two files with similar names.

**Shadowed root** — two search roots containing the same file under one logical name. The higher
precedence root wins, and mapping an existing path to a name that resolves to a different file under
that precedence is an error rather than a rename.

**Isolated root** — an input file outside all search roots, spelled as a fully qualified path. Allowed
only without imports; otherwise an explicit `-I` is required.

**Deferred parsing** — a parser mode where named types are left unresolved and produce no warnings,
because sibling files may not have been read yet. Linking them is the next step.

**Import edge (`ImportEdge`)** — a link "this file imports that file" carrying a pointer to the target,
the dependency index, and the modifier (`public` / `weak` / normal). At this stage it is a file graph,
not a type graph.

**`weak` and `public` imports** — modifiers from the descriptor. In this change a `weak` import must
exist just like a normal one, and `public` is merely preserved on the edge: re-export semantics are not
implemented yet.

## Appendix B: File and test map

New code: `logical_paths.cpp` 138 / `.hpp` 39, `file_paths.cpp` 1362 / `.hpp` 249,
`proto_loader.cpp` 815 / `.hpp` 68, `schema.cpp` +312 and `schema.hpp` +36.

Changed existing files: `CMakeLists.txt` (the `easypb_proto_loader` target, the third file in
`easypb_schema`, the `EASYPB_EXTENDED_TESTS` option), `xmake.lua` (the same two target changes; no
extended test profile there, since xmake does not register tests), `codegen/main.cpp` (the three edits
listed above), `tests/CMakeLists.txt` (test registration), the CI workflow (`extended-tests`), and the
documentation (`codegen/BUILDING.md`, the loader section in `codegen/INTERNALS.md`, the document index
in `codegen/README.md`, and the new `codegen/LOADER.md` that you are reading).

Tests and fixtures:

| What | Where | Size |
|---|---|---|
| Binder and logical names, linked against `easypb_schema` only | `tests/codegen/imports/test_graph.cpp` | 649 lines, 19 functions |
| Exhaustive enumeration of all graphs up to four files (66,066 graphs) | `test_graph_exhaustive.cpp` | 190 lines |
| Larger randomized graphs (`EASYPB_EXTENDED_TESTS` only) | `test_graph_extended.cpp` | 286 lines |
| Loader: 58 functions, fixtures, Windows variants | `test_loader.cpp` | 2606 lines |
| Diamond and search-precedence fixtures | `tests/codegen/imports/data/loader/` | 7 `.proto` files |

Registered CTest names: `codegen.imports.graph`, `codegen.imports.graph.exhaustive` (label
`property`), `codegen.imports.graph.extended` (`extended;property`), `codegen.imports.loader`,
`codegen.imports.loader.extended` (`extended;filesystem;stress`), `codegen.imports.loader.utf8`.

Loader test functions worth knowing: `test_diamond_caching_and_counters`,
`test_search_precedence_first_wins`, `test_search_precedence_error_stops_before_later_root`,
`test_missing_unused_import_fails`, `test_missing_weak_import_fails`,
`test_shadowed_root_reports_error`, `test_root_deduplication_preserves_order`,
`test_nested_cycle_reports_root_chain`, `test_failed_load_leaves_empty_result`,
`test_isolated_root_with_resolvable_imports_fails`.

One observation on proportion: `test_loader.cpp` holds about twice as many functions as the loader
itself. Those tests are not coverage for its own sake — they are an executable record of which case,
alias, and precedence combination is permitted, so roughly half of their value is pinning down the
decisions listed under "Beyond the plan".

## Appendix C: Running the checks

The loader builds in the ordinary full configuration. The extended set needs a separate build with
`EASYPB_EXTENDED_TESTS=ON`, and CI runs it once per operating system (Linux, Windows, macOS) rather
than across the whole compiler matrix. Run the checks through CTest only: the loader test creates
working directories under `build/`.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build -R codegen.imports --output-on-failure

cmake -S . -B build-extended -DCMAKE_BUILD_TYPE=Release -DEASYPB_EXTENDED_TESTS=ON
cmake --build build-extended
ctest --test-dir build-extended -L extended --output-on-failure
```

## Conclusion

The change closes exactly one task — find the files and prove that the import graph makes sense — and
does it for two inputs at once: source files on disk and a descriptor set. Its difficulty is not in the
algorithm (two `std::map`s, a depth-first search, and string comparison) but in the number of places
that must decide what is an error and what is a success: root precedence, file identity, the order of
checks, and the state of the result after a failure. On top of that task the change carries one
voluntary policy — Windows case and path folding — which the plan does not require and which is the
main candidate for a reduction.
