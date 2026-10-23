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

   This one step has an exception, and only this one: **MSVC cannot compile
   the accepted header.** The accepted nesting is printed as a chain of
   nested struct definitions (`struct M0 { struct M1 { … }; };`), and MSVC's
   compiler limit is 16 levels of them — C1046, *compiler limit: struct
   nested too deeply* — so it refuses at the 17th. Every MSVC lane of
   2026-10-22 died in the build of the runtime checker, and with it the
   whole Windows half of the CI: the build failure ends the job before
   ctest runs.

   The **limit itself does not move**: it stays the reference compiler's own
   (31 accepted, the 32nd refused), and the generator keeps printing the
   nesting the way it does. What the MSVC profile gives up is the compile of
   that one header and the runtime check that reads it. The other two things
   need no compiler and still run there: both input paths agree byte for
   byte, the 32nd level is refused, the deep name is refused. Nested types
   themselves stay covered on MSVC by the nested lane, which is shallower.

   The driver is told which profile it runs in (`COMPILE_ACCEPTED`), and
   says in its output which steps it skipped.
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
python tests/codegen/limits/data/make_descriptors.py [--protoc PATH]
```

The script needs `protoc` (30.1 is the oracle this project measures against)
and writes the `.proto` and `.pbs` files: a 31-level schema and a 32-level
schema are not writable by hand, and the pair is the whole point of the lane.

Running the lane never needs `protoc`: the fixtures are committed.

## The lanes it joins

| Test | Where |
| --- | --- |
| `codegen.limits` | this lane: generate, compile, run, refuse |
| `codegen.imports.output` | the publisher's name check, the bounded directory creation, the writer's share mode, the NUL refusal and the prefix translation |
| `codegen.imports.output.cwd` | the Windows fault-injected working-directory refusal (`S7b`, Windows only) |
| `codegen.imports.output.posix-writer` | the POSIX writer loop: `EINTR`, zero return, 64 KiB cap (`S7b`, POSIX only) |

The lane runs in every build profile: the full C++17 tree, the full C++11
tree, the lite (parser-off) tree, and the legacy toolchains. The `W-46`
section itself runs only on Windows and, for its input half, only where the
proto parser is available.

The MSVC profile runs the same lane with the two steps it cannot do left out
— the compile of the accepted header and the runtime check that reads it (see
"the header compiles" above). Every other step of the lane, and every other
test of the tree, runs there as everywhere else.
