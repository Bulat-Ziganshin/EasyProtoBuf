# Source loading and the import graph

This document explains what the loader implements and why it works the way it does. It is written as
prose on purpose: it is meant to be read end to end, not to serve as a specification or a requirement
checklist. It complements [INTERNALS.md](INTERNALS.md), which maps the source layout, and
[parser/README.md](parser/README.md), which owns the parser-level API.

The loader is a small, textual model of the reference compiler (`protoc`) plus its own diagnostics: it
finds the files, assigns logical names, and proves that the import graph makes sense. Every rule below
has a counterpart in protobuf's `compiler/importer.cc` and `compiler/command_line_interface.cc`, except
where "Deliberate differences" says otherwise.

## Summary

The loader turns an ordered list of search roots (`-I`) plus a list of input files into a `SchemaSet`
holding every file, transitive imports included, each exactly once, with an already validated import
graph: no missing imports, no cycles, and no chain deeper than the documented bound (1 000 files).
The same graph is validated for descriptor-set input as well, with no filesystem access at all. Types
and visibility (`public import` as re-export) are still not resolved; that is the next step.

## The contract, and where it comes from

Every rule below is observable behavior, and each one has a counterpart in the reference compiler:

- **Roots are used in caller order, first match wins.** `DiskSourceTree` keeps its mappings in
  insertion order and opens the first one that yields a file. An empty root list means the current
  working directory, and an empty entry means the same thing, exactly like the default `proto_path`.
- **An existing file as an argument is a disk path; anything else is a logical name.** protoc maps a
  disk path to its virtual name with `DiskFileToVirtualFile` and treats an unresolvable argument as a
  virtual name instead.
- **An input that names an existing file under no root is retried as a logical name.** This is the
  reference compiler's `NO_MAPPING` answer: `VirtualFileToDiskFile` is tried before the run gives up
  (`command_line_interface.cc:1721-1741`). A stale copy in the working directory therefore cannot
  hide the file the roots name, and the logical name stays the spelling, so the output name does too.
- **An input that lies under no root and is no logical name is an error.** protoc reports
  `File does not reside within any path specified using --proto_path (or -I)` for the same state; a
  relative spelling gets the loader's "lies outside all search roots" sentence, while an absolute
  spelling keeps protoc's sentence and additionally needs an explicit root (U-59). A spelling with no
  virtual reading (absolute, a parent reference, a backslash) never reaches the retry.
- **The mapping is textual.** Prefix comparison is byte-wise and case-sensitive, with a component
  boundary, mirroring `ApplyMapping`. A root spelled differently does not contain the file, even where
  the filesystem would open it; no case folding, no file identity. Roots and arguments are normalized
  before that comparison, which is one place where the loader is deliberately more permissive than
  `CanonicalizePath` — see the differences section below.
- **A shadowed input is reported, never renamed.** When the mapped logical name resolves to a
  different file under the ordered roots, the load stops. protoc's `DiskFileToVirtualFile` returns
  `SHADOWED` for the same state and prints an "Input is shadowed in the --proto_path by ..."
  diagnostic.
- **Windows disk access is long-path aware.** Every filesystem call converts the stored UTF-8
  spelling to the prefixed wide form (`\\?\` for a drive path, `\\?\UNC\` for a network path) that the
  reference compiler's io_win32 layer also uses. The prefix is what makes a tree deeper than `MAX_PATH`
  reachable without a manifest and without a registry setting, and both compilers apply it to every
  path rather than only to long ones, because it is also what stops Win32 from trimming trailing dots
  and spaces. Because the prefix disables that parsing, `.` and `..` have to be resolved lexically
  before the call — which they are, in the same normalization the root mapping uses. A relative
  spelling is therefore joined with the working directory textually instead of through the platform
  resolver, which would trim a trailing dot or space off the last component *before* the prefix could
  protect it and answer one name with a different file; `leaf.proto.` and `leaf.proto` are two names
  here, as they are for the reference compiler (probed: the literal spelling compiles, the trimmed one
  reports `File not found`). Drive-relative and rooted spellings still go through the platform
  resolver, whose normalization of their last component is the one corner this rule does not cover.
  The working directory that a relative spelling is joined with is capped by Win32 itself:
  `SetCurrentDirectoryW` accepts a spelling of at most 258 characters and refuses longer ones with
  `ERROR_FILENAME_EXCED_RANGE`, and the prefix, being four characters of that same budget, cannot
  lift the limit — so no process can be *in* a directory beyond `MAX_PATH`. A long absolute path therefore only ever comes from a long relative spelling under a
  shorter directory — exactly what the deep-path test builds — and the suite measures the limit
  instead of assuming it, taking the branch if a platform ever lifts it.
- **Absence advances the search, a denial stops it.** protoc stops on `EACCES` (with
  "Read access is denied for file: ...") and treats every other failure as absence. The loader keeps
  that split and adds no identity probe of its own. On Windows the layer that notices differs: an
  attribute query answers from the directory entry and does not evaluate the file's DACL, so an ACL
  denial surfaces when the file is read, and a read failure fails the whole load instead of falling
  back to another root — the same guarantee, reported one step later. protoc sees it earlier only
  because its search opens every candidate.
- **Every import must exist, weak imports included.** protoc's `Importer` sets
  `EnforceWeakDependencies(true)` for source input, so a missing weak import is an error there too.
- **A cycle is an error with the chain.** protoc reports `File recursively imports itself: a -> b -> a`
  with a source position; the shared binder reports the discovery chain from the root.
- **One file per logical name.** The descriptor pool of protoc is keyed by file name as well, so a
  diamond import or a repeated input parses once and the descriptor stores the logical name.
- **Two files under one logical name, and two names for one file, are not detected here.** protoc has
  no file-identity concept either: duplicate definitions are reported later by the linker step, and two
  names for one physical file simply load twice.

## Three roles in four places

The split is not modularity for its own sake. The project has a build profile without the source
parser (descriptor sets only), and a plugin mode is planned; neither should pull in filesystem code.

| Role | Where | Lines | Who links it |
|---|---|---|---|
| Logical name rules | `logical_paths.{hpp,cpp}` | 45 + 132 | every profile, including descriptor-only and plugin |
| Storage and graph ownership | `bind_import_edges` and helpers in `schema.{hpp,cpp}` | 289 + 509 | every profile |
| Disk adapter | `file_paths.{hpp,cpp}` | 129 + 620 | full builds only |
| Source loader | `proto_loader.{hpp,cpp}` | 79 + 598 | full builds only |

In CMake terms:

- `easypb_schema` = `schema.cpp` + `schema_semantics.cpp` + `logical_paths.cpp`;
- `easypb_proto_parser` = the parser, on top of `easypb_schema`;
- `easypb_proto_loader` = `proto_loader.cpp` + `file_paths.cpp`, on top of both, and created only when
  `EASYPB_CODEGEN_WITH_PROTO_PARSER` is on.

Why the name rules are separate rather than private to the loader: a logical name is the shared
contract of three frontends — source input, descriptor-set input, and future plugin input — and it must
be validated identically for all of them. That means the validation has to work without the parser and
without the filesystem, or descriptor-set input could not call it at all.

## The path of one call

Everything starts in `load_source_files(options, roots, result, error)`: clear the result, normalize
the roots, then for every input argument call `resolve_root` and follow its import chain, append the
loaded file to `targets`, and call `bind_import_edges` once at the very end. The order is not
incidental: until every file is loaded there is no graph to validate.

### Turning an argument into a logical name (`resolve_root`)

An argument is first tried **as a disk path**, and only if no such file exists is it treated as a
logical name. Disk first is deliberate: a spelling that names a real file stays a disk path even when
it would also be a valid logical name.

An **absolute argument must name an explicit search root**. The implicit working directory (the
default when the caller passes no roots) serves relative spellings only: protoc's default mapping is
the empty spelling `"."`, and its textual comparison never contains an absolute path, so an absolute
input without `-I` is refused with the reference sentence (`File does not reside within any path
specified using --proto_path (or -I). You must specify a --proto_path which encompasses this file.`)
instead of inventing a logical name from the physical location (review 1027, U-59). Roots and
arguments are normalized before the comparison, so an explicit root does contain an absolute file
under it even when the root was spelled relative — the normalization difference recorded below.

When the file exists, `map_disk_to_logical` walks the search roots in order, strips the first root that
contains the path byte-wise, and produces a logical name. The name is then **re-verified**: if that
logical name resolves to a different file under the same ordered roots, this is reported as shadowed
instead of being treated as a rename. A remainder that is not a valid logical name is remembered and
reported if no other root yields a valid mapping — such a file must not fall through to the "outside
all roots" answer.

When the argument is not a disk path, `resolve_logical` walks the roots in precedence order and the
first one that has the file wins. **A denied access stops the search** and does not advance to the next
root; only absence may advance it, exactly like the reference compiler. A directory is never a
candidate, so hitting one advances the search, which is also what protoc's `OpenVirtualFile` does when
`OpenDiskFile` fails on one. An argument that is neither an existing file nor a valid logical name
fails as an invalid root spelling (protoc reports it as unresolvable; both are hard errors that load
nothing).

An argument that names an existing **directory** is reported as a directory when it lies under a root
— equality counts, so `codegen -I root root` and `codegen .` name the root itself — with
`root "..." is a directory, not a schema file (search root "...")`, and as the ordinary outside-roots
failure when it does not; an absolute directory outside every root keeps the reference sentence
instead. protoc distinguishes the same two cases — `Input file is a directory.` from the failed open,
and `File does not reside within any path specified using --proto_path (or -I)` when no mapping
applies — and, like here, never retries such a spelling as a virtual name.

An **empty argument** is refused before anything else happens, with a message that says the file name
is empty. It is not a spelling to interpret, and it is not an empty logical path either. protoc treats
it the same way — its CLI answers an empty argument with "it is not valid input" and exits — so only
the wording differs. An empty *search root* entry is a different thing altogether: it means the working
directory, exactly like the default `proto_path`, and the search continues from there.

### Loading one file: three checks in order (`begin_frame`)

The chain is followed with an **explicit heap worklist**, not one C++ call frame per import edge
(`U-05`/`U-06`): a recursive walk ended the process with `STATUS_STACK_OVERFLOW` (`0xC00000FD`) and
an empty stderr on a chain of ~1 950 files, and the import graph is untrusted input. The walk keeps
the recursive shape - a frame is expanded in declaration order, depth first, and every diagnostic and
read order is unchanged - but the depth costs heap, not the process stack.

Every frame goes through the same three checks, in the same order:

1. **The logical name is already present** — return silently, so a repeated command-line root or a
   diamond import is not an error, exactly like the descriptor pool keyed by file name.
2. **The chain depth is within the bound** — `max_import_chain_depth()` files, see
   [The import-chain bound](#the-import-chain-bound). A file one level deeper is refused before it
   is read.
3. **The slot is reserved in the `SchemaSet` before imports are followed.** This is what makes both a
   diamond and a cycle safe: no file is parsed twice and no walk can be infinite. The slot is empty
   at this point; parsing fills it. Reading and parsing happen in a separate helper
   (`read_and_parse_source`): the file is read into a local buffer and parsed deferred
   (`defer_type_resolution = true`); the slot gets `file.name` = the logical name and `physical_name`
   = the disk path; the buffer dies before dependencies are followed.

Check 3 answers "why deferred, and why two names". The source must not outlive parsing, because
otherwise it is retained for the whole walk while the import depth is not known in advance. At the
same time `physical_name` is needed afterwards for diagnostics, and `file.name` must hold the logical
path: generated include names come from it, and it is the key that matches files against each other.

### Reading one file (`read_disk_file`)

On POSIX the file is opened once and read through the descriptor: `open`, then `fstat` to classify
the entry that was actually opened, then `read` in a loop that retries `EINTR` and treats every short
count as progress rather than as the end. The iterator-based read this replaces could not report a
failure at all — `istreambuf_iterator` never sets the stream state, so the post-read `fail()` guard
was dead code and a short read returned truncated bytes with success — and the stat probe ran before
the open, leaving a window in which the entry could change (review 1027, U-27). A failing read now
clears the buffer and reports the errno value and text (`cannot read file (errno N: text)`), the
platform detail `OPTIONS.md` promises (U-37). The Windows branch already reads in a `ReadFile` loop
and is unchanged: it reports `Win32 error N`.

### Imports (`follow_import`)

The walk follows the descriptor's `dependency` list, which is authoritative. The modifier comes from
`weak_dependency` / `public_dependency` by index, and the parser's `imports` vector is used **only** as
a source of positions for messages. Every import is loaded, including ones whose types nobody
references, and a `weak` import has the same existence requirement as a normal one in this complete
mode.

### The import-chain bound

A discovery chain may be at most **1 000 files** deep (`max_import_chain_depth()`, owner decision 10
of review 1027). 1 000 files are accepted; the 1 001st is refused with

```text
f1000.proto: import chain depth 1001 exceeds the limit of 1000 files (imported by "f999.proto"; the chain starts at "f0.proto")
```

The sentence names the depth, the limit, the offending file, the import that asks for it and the root
of the chain - and deliberately not the whole chain, whose length is the problem. The bound is a
policy limit and not a reference behaviour: protoc compiles chains of 3 000-3 500 files, so a schema
this project refuses is one protoc accepts. It exists because the import graph is untrusted input and
everything that follows it - the walk, the linker's visibility sets, the generated include graph -
costs time and memory per level. It is the analogue of the message-nesting limit (31 levels, see
[GENERATED_CODE.md](GENERATED_CODE.md#current-limitations-and-semantic-differences)): a documented
refusal at a documented depth instead of a crash or an unbounded allocation at an undocumented one.
The linker enforces the same bound on the graph it receives, so a descriptor set - which arrives
without a filesystem walk - is refused identically (`schema_linker.cpp`, `validate_import_chain_depth`).


### Final graph validation (`bind_import_edges`)

Edges are built with a stable pointer to the target `SchemaFile` inside the `SchemaSet` (files live on
the heap, so addresses do not move as the vector grows), plus the dependency index and the modifier.
Cycle detection then runs as an iterative depth-first search with an explicit stack, so the message can
report the chain from the root to the declaration that closes the cycle instead of merely stating that
a cycle exists. The binder also rejects a repeated name in one dependency list and an inconsistent
`public`/`weak` index, and it runs for descriptor-set input as well, where no filesystem exists.

## Five invariants

- **Descriptors outrank `imports`.** The `dependency` / `public_dependency` / `weak_dependency` lists
  are the source of truth; the `imports` vector only supplies coordinates for diagnostics. This is why
  the binder also works on descriptor-set input, where `imports` does not exist at all.
- **Mapping is textual.** Roots, prefixes and spellings are compared as bytes; nothing consults the
  filesystem to decide containment, and nothing canonicalizes case, inodes or reparse points. This is
  the same trade-off protoc documents: two paths that look equivalent (absolute versus relative) are
  simply not equivalent to the compiler.
- **Caching happens before the walk continues, and "this is a cycle" is decided in the binder.** The loader
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

**In the graph:** an import that is absent on disk, even when no type references it; a cycle; a chain
deeper than the import-chain bound; a file under a search root whose remainder is not a valid logical
name; a shadowed input; a relative argument that lies outside all search roots (after the logical-name
retry); an absolute argument outside every explicit root (the reference sentence above); a directory
argument, or an empty one; a denied access while resolving or reading.

Import failures carry their own `DiagnosticCode`: `DIAGNOSTIC_INVALID_IMPORT`,
`DIAGNOSTIC_MISSING_IMPORT`, `DIAGNOSTIC_IMPORT_CYCLE`. They are distinct values, and a test asserts
that they stay distinct.

Messages use two chain spellings on purpose: the binder prints its discovery path unquoted
(`import cycle: a.proto -> b.proto -> a.proto`), while the loader appends the selected root in quoted
form (`; chain "root.proto" -> "middle.proto"`). These are two different facts — the graph path and the
discovery path — not an inconsistency.

## Deliberate differences from the reference compiler

Everything here is diagnostic quality, spelling strictness, or a more forgiving reading of a spelling.
Two bullets accept a spelling the reference compiler cannot resolve — parent references, and a network
path past the path cap — and neither can select a different file than the caller named.

- **`..` is resolved, and drive-relative spellings are resolved outside the root list.**
  `lexical_absolute` turns paths into an absolute form before the prefix comparison: it resolves
  relative spellings against the working directory, resolves `..` lexically, and asks the platform
  path API for drive-relative (`C:x`) and rooted (`\x`) Windows forms. A drive-relative spelling is
  never accepted as a *search root*, though: the root list is checked before that normalization and
  the form is refused, because the per-drive current directory would let one spelling name different
  trees across runs (review 1027, U-60); protoc refuses it as a `--proto_path` as well. protoc's
  `CanonicalizePath` only converts backslashes,
  drops `.` components and collapses slashes — its own comment says handling `..` is "punted" — and
  `ApplyMapping` then rejects any part still containing a parent reference. Consequence: a spelling
  such as `-I foo foo/../foo/x.proto` maps here and is an error there. It can never select a different
  file: the mapping still has to pass the shadow check and the logical-name rules. Matching protoc
  exactly would mean rejecting such spellings.
- **Only the `-I dir` shape of a search root exists here.** `SourceLoaderOptions.proto_paths` holds
  disk directories, so the mapping is the empty-virtual-prefix form of `ApplyMapping`; protoc also
  accepts `--proto_path=VIRTUAL=DISK`, which pairs a virtual prefix with a disk directory. Nothing in
  this API can express that form yet, and protoc's `--proto_path` alias parsing is not represented
  either: argument spelling belongs to the CLI. The CLI does own the reference's no-input refusal
  ("Missing input file.") and its empty-`-I` rule; both live in `main.cpp`/`OPTIONS.md`, and the
  loader only sees the resulting root list.
- **Codes, positions and context.** The loader reports its own `DiagnosticCode`, source positions, the
  searched roots, the selected root chain and an `-I (--proto_path)` hint. protoc reports a shorter
  file-plus-position message and no machine-readable code. The directory message above is the same
  kind of addition: both compilers refuse a directory input, and this one says which it is.
- **Stricter import spellings.** In addition to the four virtual-path rules protoc enforces
  (backslashes, consecutive slashes, `.`, `..`), a logical name may not be absolute, carry a drive
  prefix, contain quotes or control bytes, or be malformed UTF-8. protoc would report such a name as a
  missing file (and on POSIX it would even accept a backslash as an ordinary character); this project
  rejects it with a reason for every frontend, including foreign descriptor input.
- **A NUL guard.** A spelling with an embedded NUL fails loudly instead of addressing a truncated
  prefix, both for CLI roots (`disk_file_status`) and for file reads (`main.cpp`). protoc's narrow
  calls simply truncate at the NUL and report a missing file.
- **A network path beyond the path cap.** protoc's long-path layer drops the leading separator pair of
  a UNC spelling while normalizing, so its prefixed result no longer addresses the share; this loader
  keeps the share in the `\\?\UNC\` form, the only prefixed spelling that can name it. A drive-absolute
  path is prefixed exactly as the reference compiler does it.

## What is deliberately absent

These are things a reader might expect from a loader like this and will not find. Each is a decision
with a consequence, not an oversight.

- **Windows case and Unicode folding.** There are no per-directory case-sensitivity queries
  (`FileCaseSensitiveInfo`), no uppercase table, no entry-by-entry confirmation of folded prefix
  matches, and no lexical equivalence of spellings. protoc does byte-wise prefix comparison plus a
  component-boundary check with no folding at all, even on Windows. Consequence: a disk path spelled
  with a different case for its root does not map, exactly like protoc, instead of being folded onto
  the same file (a relative spelling reports "outside all search roots"; an absolute one keeps the
  reference sentence). A different case in the *file name* still opens on Windows, because
  the filesystem is case-insensitive and the loader asks it directly.
- **The isolated mode.** There is none: a fully qualified, import-free file outside every search root
  does not load on its own. protoc has no such mode either — an input must live under some
  `--proto_path` — so an absolute input needs an explicit `-I <dir>`; the implicit working directory
  is not one, and a logical name has to be passed instead if the file is to be found through it.
- **Native file identity and duplicate-alias detection.** Identity (`st_dev`/`st_ino`, volume plus file
  index) is not queried. One physical file under two logical names loads twice, and two files under one
  logical name are not diagnosed here. Both match the reference compiler, which keys everything by name
  and lets the duplicate definitions surface later.
- **Statistics, test seams and fault-injection matrices.** There is no `LoadStatistics`, no
  status/case/identity probe hooks, no filesystem-failure or concurrency matrix, and no Windows
  `activeCodePage` manifest twin. The loader reports through its `Diagnostic` and is otherwise silent;
  `EASYPB_EXTENDED_TESTS` exists for the randomized graph stress test, which needs none of that. The
  encoding contract stated in `file_paths.hpp` — UTF-8 spellings converted to UTF-16 only at the W-API
  calls — is covered by a test that creates non-ASCII fixtures under the code page of the machine
  itself, asks that page, and uses the narrow spelling as a control; a run under a UTF-8 code page says
  so, so weaker evidence is visible instead of silent. The long-path contract next to it is covered by
  a test that builds a tree past `MAX_PATH` out of non-ASCII components, checks that the unprefixed
  spelling cannot reach the leaf there, and then loads it both through a search root and as a
  disk-path argument.

## Reading order

1. `logical_paths.cpp` — self-contained, and the right place to start.
2. `bind_import_edges` in `schema.cpp` — the graph core.
3. `load_source_files` in `proto_loader.cpp` — the whole outer contour, short enough to read first.
4. `resolve_root` and `map_disk_to_logical` — the root logic.
5. `join_disk_path`, `lexical_absolute`, `strip_search_root_prefix`, `wide_win32_path`,
   `disk_file_status` in `file_paths.cpp` — read on demand, not front to back.

## Appendix A: Glossary

**Logical name** — the relative path with `/` separators stored in `FileDescriptorProto.name` and in
the dependency lists, and reused for generated include names. It is a strict platform-independent
UTF-8 namespace: absolute spellings, backslashes, empty components, `.`/`..`, quotes, control bytes,
and malformed UTF-8 are rejected.

**Shadowed input** — an argument whose disk path maps to a logical name that the ordered roots resolve
to a different file. The higher-precedence root wins, and the mapping of that existing path is an error
rather than a rename.

**Textual mapping** — the loader's replacement for identity: a file is "the same file" only when its
logical name is the same. Nothing compares inodes, reparse points or case.

**Deferred parsing** — a parser mode where named types are left unresolved and produce no warnings,
because sibling files may not have been read yet. Linking them is the next step.

**Import edge (`ImportEdge`)** — a link "this file imports that file" carrying a pointer to the target,
the dependency index, and the modifier (`public` / `weak` / normal). At this stage it is a file graph,
not a type graph.

**`weak` and `public` imports** — modifiers from the descriptor. A `weak` import must exist just like a
normal one for source input, and `public` is preserved on the edge: re-export semantics are not
implemented yet.

## Appendix B: File and test map

| Module | Lines |
|---|---|
| `logical_paths.cpp` / `.hpp` | 132 / 45 |
| `schema.cpp` / `.hpp` (storage plus the binder) | 509 / 289 |
| `file_paths.cpp` / `.hpp` | 620 / 129 |
| `proto_loader.cpp` / `.hpp` | 598 / 79 |

Tests and fixtures:

| What | Where | Size |
|---|---|---|
| Binder and logical names, linked against `easypb_schema` only | `tests/codegen/imports/test_graph.cpp` | 659 lines |
| Exhaustive enumeration of all graphs up to four files (66,066 graphs) | `test_graph_exhaustive.cpp` | 190 lines |
| Larger randomized graphs (`EASYPB_EXTENDED_TESTS` only) | `test_graph_extended.cpp` | 286 lines |
| Loader: 44 functions, fixtures, denial, Unicode, deep-path and name-edge cases | `test_loader.cpp` | 2364 lines |
| Diamond and search-precedence fixtures | `tests/codegen/imports/data/loader/` | 7 `.proto` files |

Registered CTest names: `codegen.imports.graph`, `codegen.imports.graph.exhaustive` (label
`property`), `codegen.imports.graph.extended` (`extended;property`), `codegen.imports.loader`.

Loader test functions worth knowing: `test_diamond_caching`, `test_search_precedence_first_wins`,
`test_denied_access_stops_search` (POSIX `chmod` and Windows deny-ACE variants),
`test_shadowed_root_reports_error`, `test_disk_root_with_invalid_logical_name_fails`,
`test_directory_argument_is_reported_as_directory`, `test_empty_input_argument_is_rejected`,
`test_missing_unused_import_fails`, `test_missing_weak_import_fails`,
`test_nested_cycle_reports_root_chain`, `test_two_names_of_one_file_load_twice`,
`test_case_variant_root_spelling_is_outside_roots`, `test_failed_load_leaves_empty_result`,
`test_non_ascii_paths_do_not_depend_on_the_process_code_page`,
`test_wide_conversion_applies_the_long_path_prefix`, `test_non_ascii_paths_beyond_max_path`,
`test_trailing_dot_and_space_names_stay_literal`,
`test_relative_spelling_follows_the_working_directory`.

## Appendix C: Running the checks

The loader builds in the ordinary full configuration.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build -R codegen.imports --output-on-failure

cmake -S . -B build-lite -DBUILD_TESTING=ON -DEASYPB_CODEGEN_WITH_PROTO_PARSER=OFF
cmake --build build-lite && ctest --test-dir build-lite --output-on-failure

cmake -S . -B build-extended -DCMAKE_BUILD_TYPE=Release -DEASYPB_EXTENDED_TESTS=ON
cmake --build build-extended && ctest --test-dir build-extended -L extended --output-on-failure
```

Run the loader suite through CTest only: it creates working directories under the build tree.

## Conclusion

The loader does one thing — find the files and prove that the import graph makes sense — and it does it
the way the reference compiler does: a textual root mapping, a shadow report for disk-path inputs, a
denial-only search stop, name-keyed caching, and a long-path-aware Windows conversion. What it adds is diagnostics: codes, positions, the
searched roots and import chains, plus a stricter logical-name validator shared by every frontend. What
it leaves out is every mechanism the reference compiler does not have — case folding, native identity,
an isolated-root convenience, statistics and test seams — so that each behavior can be justified by
`protoc` or by a diagnostic this project deliberately improves on.

The output publisher was brought to the same position by the plan-5 trim (`fbdcc0e`, 2026-10-11). It had grown the analogous
mechanisms — output-root containment, measured case-fold collision detection, native file identity and
hardlink detection, and atomic per-file replacement through a temporary sibling — at 703 code lines
against the reference compiler's roughly 80 for the same job. They were withdrawn by owner decision;
see `header_output.hpp` for the measurements that justified each one, and for the name check that
survives: the logical-name rule that `generated_header_path` applies, and — since 2026-10-21 — the
same rule inside the publisher, so a hand-built output name cannot escape the output root either.
