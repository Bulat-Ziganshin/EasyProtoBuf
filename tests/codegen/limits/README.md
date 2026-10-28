# The limits lane — the boundaries of untrusted input

One lane for the three rows of the plan-5 follow-up's session `S3`: `W-01` (no
nesting-depth limit: a deep schema ends the process with a stack overflow
instead of a message), `W-30` (the publisher does not validate the output
name, so a library caller writes outside the output root) and `W-32` (the
output tree is created without a bound, one directory per path component).
Every row is a counter-example against `protoc` 30.1, and the lane proves the
fix in the three ways the session's brief names.

Since `S7b` the lane also covers `W-46`: an explicit `\\?\` (or `//?/`) prefix
in `--out-dir` or in an input spelling is a transport spelling, not part of the
name. The Windows-only section at the end writes the same fixture through the
plain and the prefixed spellings and requires one header byte for byte; the
input half runs under `FULL_BUILD`.

Since the 1027 review it also covers the two graph bounds: the by-value
dependency chain of `CR-35` (12 000 messages, accepted) and the import-chain
bound of `U-05`/`U-06` (1 000 files accepted, 1 001 refused, 5 000 and 20 000
refused without a crash). Both are documented below.

## The three things every row needs

1. **Both input paths.** The source path (`.proto`) and the descriptor path
   (`.pbs`) must agree: the same schema is accepted by both or refused by
   both, and the two paths produce the **same header** for the accepted one.
   The driver compares the two headers byte for byte (the source half runs
   only in a parser-on profile).
2. **The header compiles.** A generated header that does not compile is a
   failure; "the tool exited 0" is not a pass. The driver compiles the
   generated header with the tree's own compiler and the tree's own standard
   (`-std=c++17` in the full profile, `-std=c++11` in the C++11 profile), and
   the build compiles the same header into the runtime checker.

   This one step has exceptions, and nothing else does. **MSVC cannot
   compile the accepted header.** The accepted nesting is printed as a
   chain of nested struct definitions (`struct M0 { struct M1 { … }; };`),
   and MSVC's compiler limit is 16 levels of them — C1046, *compiler limit:
   struct nested too deeply* — so it refuses at the 17th. Every MSVC lane of
   2026-10-22 died in the build of the runtime checker, and with it the
   whole Windows half of the CI: the build failure ends the job before
   ctest runs.

   **Clang 22–23 have a different wall — speed.** A declaration whose type
   is spelled as a long nested-name-specifier costs exponential time there
   (roughly doubling for every level past ~20; measured 2026-10-23 through
   the Compiler Explorer machines: 0.8 s and 4.7 s at depths 24/26 against
   35–68 ms for Clang 20/21, and LLVM main resolves the same source in
   ~5 ms). The struct definitions alone compile in milliseconds — it is the
   mention of the deep type, in any form (`M0::M1::…::M30 v;`, a pointer, a
   function parameter), that explodes. The generated header carries 62 such
   declarations, so the Clang 22/23 lanes paid ~8 minutes for the runtime
   checker's TU and ~6 more for the driver's `-fsyntax-only` — per build
   type — and seven lanes were canceled at their job timeout. The skip is
   version-gated (LLVM 22.1.0 through 23.1.0) and lifts with Clang 24.

   The **limit itself does not move**: it stays the reference compiler's own
   (31 accepted, the 32nd refused), and the generator keeps printing the
   nesting the way it does. What the skipping profiles give up is the
   compile of that one header and the runtime check that reads it. The
   other two things need no compiler and still run there: both input paths
   agree byte for byte, the 32nd level is refused, the deep name is
   refused. Nested types themselves stay covered on MSVC by the nested
   lane, which is shallower.

   The driver is told which profile it runs in (`COMPILE_ACCEPTED`) and why
   (`SKIP_REASON`), and says in its output which steps it skipped.
3. **The refusal is a diagnostic.** A non-zero exit, an empty stdout and a
   stderr that names the offending thing. A crash is a failure, not a
   refusal: the whole point of the rows is that a deep input used to end the
   process with `0xC00000FD` and an empty stderr.

The accepted nesting also has to **carry the schema's data**: the runtime
checker builds the innermost message of the 31-level chain, writes its field,
compares the **bytes** on the wire and reads them back.

## The fixtures

| Fixture | Path | Row |
| --- | --- | --- |
| 31 levels of nested messages (accepted) | `data/nesting-31.proto` / `.pbs` | `W-01` |
| 32 levels of nested messages (refused) | `data/nesting-32.proto` / `.pbs` | `W-01` |
| a logical name with 101 components | `data/deep-name.pbs` | `W-32` |
| 12 000 messages in a by-value dependency chain (accepted) | `data/dependency-chain.proto` / `.pbs` | `CR-35` |
| 1 000 files in one `import public` chain (accepted) | `data/public-chain-1000.pbs` | `U-05`, `U-06` |
| 20 000 files in one `import public` chain (refused) | `data/public-chain-20000.pbs` | `U-05`, `U-06` |
| 4 096 explicit proto3 `optional` fields (accepted) | `data/wide-optional.proto` / `.pbs` | `V-21`, `V-22` |

The two nesting depths are the reference compiler's own boundary: its
`MaxMessageDeclarationNestingDepth()` is 32 (`descriptor.h:3045`) and it
refuses the **32nd** level with *"Reached maximum recursion limit for nested
messages."* — the sentence this project uses for the same shape. `protoc` 30.1
accepts `nesting-31.proto` and refuses `nesting-32.proto` with exactly that
sentence, and the lane's verdicts match it.

`nesting-31.pbs` is protoc's own encoding of the `.proto` beside it.
`nesting-32.pbs` is **crafted**: protoc refuses to build a 32-level
descriptor, so the fixture is encoded from the text format, which knows no
such limit. `deep-name.pbs` is a descriptor whose logical name is a chain of
101 components — the name is what drives the depth of the output tree, so it
is the input the path bound has to refuse.

Nothing in the lane creates a deep directory tree: the deep name is refused
at the mapping layer, before the publisher creates anything, and the driver
asserts that the output root is still empty.

## The dependency-chain depth (`CR-35`)

The nesting limit bounds how deeply a message may be **declared**. A second,
independent axis is how deeply messages may **depend** on each other: 12 000
top-level messages, each holding the next one by value, has nesting depth 1
and a dependency graph 12 000 edges deep. That is the shape the recursive
cycle detector and topological sort used to walk with one C++ call frame per
edge, so the process ended with `0xC00000FD` (STATUS_STACK_OVERFLOW) instead
of a diagnostic - on both input paths.

The driver generates the chain from the descriptor and, in a parser-on
profile, from the source, and asserts three things:

1. both runs exit 0 with empty stdout and stderr (`run_ok` treats the
   overflow code as a failure, so a regression cannot pass silently);
2. the descriptor path is deterministic: the same input twice produces one
   header byte for byte;
3. the source and descriptor headers agree byte for byte.

The accepted header is **not compiled**: it is a 9.5 MB chain of 12 000
struct definitions, and the claim is that the generator survives the walk.
The chain is not a crash-only fixture either: the driver checks that both ends
of the chain are in the header (the walk is dependency-first, so the last
declaration comes first), and the byte-for-byte comparison with the source
path proves the two frontends emit the same 12 000 declarations.

The walk uses an explicit heap stack in both places, so the depth is bounded
by memory rather than by the process stack. There is no documented depth
bound to trip: an accepted schema stays accepted.

## The import-chain bound (`U-05`, `U-06`)

The import graph is the second untrusted axis, and it has its own bound: a
discovery chain may be at most **1 000 files** deep. 1 000 files are accepted,
the 1 001st is refused with

```text
d4999.proto: import chain depth 1001 exceeds the limit of 1000 files (imported by "d4998.proto"; the chain starts at "d3999.proto")
```

which names the depth, the limit, the offending file, the import that asks for
it and the root of the chain. The descriptor path has no filesystem walk to
attribute a chain to a root, so its sentence is the same without the
parenthetical: `c0.proto: import chain depth 20000 exceeds the limit of 1000
files`. The bound is the import-graph analogue of the nesting limit above, and
it is a protective limit rather than a reference behaviour: protoc compiles
chains of 3 000–3 500 files, so a schema this lane refuses for depth is one
protoc accepts. It exists because both walks used to recurse once per import
edge (the loader ended the process with `0xC00000FD`, `STATUS_STACK_OVERFLOW`,
on a chain of ~1 950 files; the linker's `public_exports` did the same on
~20 000 `import public` links and used ~3 GB on 8 000) and because every
per-level structure that follows the chain costs memory.

The lane asserts the boundary on both input paths:

| Case | Door | Verdict |
| --- | --- | --- |
| 1 000-file `import public` chain (`c0`…`c999`) | source (`.proto` files written into `WORK_DIR`) | exit 0, header written **and equal to the descriptor header byte for byte** |
| 1 001-file chain | source (`d3999`…`d4999`) | exit 1 with the sentence above and `d4999.proto` named |
| 5 000-file chain | source (`d0`…) | exit 1 with the same sentence — the review's crash length |
| 1 000-file `import public` chain | descriptor (`public-chain-1000.pbs`) | exit 0, header written |
| 20 000-file `import public` chain | descriptor (`public-chain-20000.pbs`) | exit 1, `import chain depth 20000 exceeds the limit of 1000 files` |

The refusals go through `run_refused`, which accepts **exit code 1 only**: a
crash returns `0xC00000FD` (or a POSIX signal) and fails the row, and the
stderr assertions below it would fail on a crash's empty stderr as well.

Two details of the fixtures are deliberate. The source chains are written by
the driver, not committed: a repository cannot carry 6 000 tracked files for
three rows, and the driver already owns its scratch tree. The two descriptor
sets are committed, because a single `.pbs` holds the whole graph and
`protoc`'s text encoder can produce it (`make_descriptors.py`); the 20 000
links are the length the review measured the crash at. The accepted chain is
**one schema on both doors** — the `.proto` files and the `.pbs` carry the same
names and the same edges — so the two headers are compared byte for byte; the
accepted header is not compiled, because a 1 000-deep public-import chain is
the subject of the walk and the bound, not of the compiler.

Measured on 2026-10-27 in the `tree-cxx17` profile, before and after (`U-05`,
`U-06`; the pre-change binary is the `build/verification/tree-cxx17` build,
whose `schema_linker.cpp` and `proto_loader.cpp` are those of `0ae9683`, the
last commit that touched them):

| Case | Before | After |
| --- | --- | --- |
| source 1 000 `import public` | exit 0, 1.05 s, 59 MB | exit 0, 1.05 s, 58 MB |
| source 1 001 | exit 0 (accepted — no bound), 1.04 s | exit 1, 1.04 s, 11 MB, the sentence |
| source 5 000 | **`0xC00000FD`**, 10.14 s, empty stderr | exit 1, 1.05 s, 11 MB, the sentence |
| descriptor 1 000 `import public` | exit 0, 1.03 s, 58 MB | exit 0, 1.04 s, 58 MB |
| descriptor 20 000 `import public` | **`0xC00000FD`**, 3.05 s, 111 MB, empty stderr | exit 1, 1.05 s, 111 MB, the sentence |

The peak working set of the accepted 1 000-file chain is unchanged: the
per-file visibility set is the cost, and the bound is what keeps it bounded
(the same shape at 8 000 files used ~3 GB before the bound existed). What the
fix removes is the crash, the 10 s spent reading 5 000 files before dying, and
the recursion depth.

## The wide optional message (`V-21`, `V-22`)

The review's two scaling findings share one shape: a proto3 message with
thousands of explicit `optional` fields. Each field carries a synthetic
one-field oneof, so the message has as many oneof declarations as fields.
`find_field_source` used to walk the whole provenance table once per field
(`V-21`), and `validate_message_oneofs` walked the whole field list once per
oneof in three separate rules (`V-22`); both are quadratic on this shape.

`wide-optional.proto` / `.pbs` is one such message with 4 096 fields and
4 096 synthetic oneofs after the real oneofs, written by `protoc` from the
source beside it. The lane requires both doors to accept it, both headers to
be identical byte for byte, and both ends of the field list (`f0` and
`f4095`) to be present. The header is not compiled: the claim is that the
semantic pass is linear in the field count and produces the same model from
both frontends, while the compile gate is already covered by the nesting
fixture and the other lanes. The fixture is a correctness control - no
assertion in the lane depends on elapsed time; the review's timings are
recorded in the step log as evidence, not as a gate.

## The long-path prefix (`W-46`)

The prefix turns off Win32 path parsing, so it is a transport spelling: the
path layer translates `\\?\C:\…` to `C:/…` and `\\?\UNC\server\share\…` to
`//server/share/…`, and re-applies the canonical prefix only at the W-API
boundary. The lane drives the same `nesting-31` fixture three times — plain,
`\\?\` and `//?/` — and compares the written headers byte for byte. Under
`FULL_BUILD` the source path is exercised with `-I` and the `.proto` argument
prefixed, and the descriptor path with the `.pbs` argument prefixed; all three
headers must equal the plain descriptor header. A mixed spelling such as
`\\?\C:/…` is accepted because the translator normalizes its separators; this
is exactly the shape `--out-dir` receives on Windows.

## Rebuilding the fixtures

```sh
python tests/codegen/limits/data/make_descriptors.py [--protoc PATH] [--check]
```

The fixture table and the generated source text are in that script; the protoc
lookup, the `descriptor.proto` lookup, the text-format encoder and the writer
are shared with the other descriptor lanes in
`tests/codegen/common/descriptor_fixtures.py`. It writes the `.proto` and
`.pbs` files: a 31-level schema and a 32-level schema are not writable by hand,
and the pair is the whole point of the lane. The same script writes the
12 000-message dependency chain and its `.pbs`, and the two `import public`
chains (1 000 and 20 000 files) as descriptor sets — those are crafted from
text format rather than compiled, because no single `.proto` expresses 1 000
files. `--check` regenerates every fixture in memory and fails on any byte that
differs from the committed file.

Running the lane never needs `protoc`: the fixtures are committed. The three
source-path chain cases write their own `.proto` files into the lane's
`WORK_DIR` when the driver runs (`FULL_BUILD` only); they are not committed.

## The lanes it joins

| Test | Where |
| --- | --- |
| `codegen.limits` | this lane: generate, compile, run, refuse |
| `codegen.imports.output` | the publisher's name check, the bounded directory creation, the writer's share mode, the NUL refusal and the prefix translation |
| `codegen.imports.output.cwd` | the Windows fault-injected working-directory refusal (`S7b`, Windows only) |
| `codegen.imports.output.posix-writer` | the POSIX writer loop: `EINTR`, zero return, 64 KiB cap (`S7b`, POSIX only) |
| `codegen.imports.output.posix-reader` | the POSIX read loop: `EINTR`, short reads, errno text and fstat directory classification (`U-27`/`U-37`, POSIX only) |

The lane runs in every build profile: the full C++17 tree, the full C++11
tree, the lite (parser-off) tree, and the legacy toolchains. The `W-46`
section itself runs only on Windows and, for its input half, only where the
proto parser is available.

The profiles that skip the compile (MSVC, and the Clang 22–23 versions) run
the same lane with the two steps they cannot do left out — the compile of the
accepted header and the runtime check that reads it (see "the header
compiles" above). Every other step of the lane, and every other test of the
tree, runs there as everywhere else.
