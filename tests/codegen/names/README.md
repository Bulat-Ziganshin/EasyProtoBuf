# The names lane — the emitter's name layer

One lane for the two rows of the plan-5 follow-up's session S2: `W-08` (a
C++ keyword in a declaration name) and `W-09` (a presence flag that collides
with a member of the same message), for the post-closure review of
2026-10-22 that found both rules incomplete (`PC-01`: a nested type collides
with a presence flag as well; `PC-02`: the escape rule is not injective), for
step B6 of the verification stage, which turned the rule into an
**occupant enumeration** (`CR-03` enumerators, `CR-04` codec names, `CR-09`
runtime namespaces, `CR-10` non-identifier names, `CR-43` the local-name
helper; `CR-44` stays accepted), and for the review of 2026-10-27, which added
the occupants that were still missing: the class's own name (`U-01`), the
runtime types under `package easypb;` (`U-02`) and the global typedefs of the
generated preamble (`U-03`).

Every row is a counter-example against `protoc` 30.1, and the lane proves the
fix in the three ways the session's brief names.

Step R11 adds the position contract of a refusal (`U-30`). The emitter throws
a positioned error for the declaration it blames, and the CLI turns it into the
`file:line:column` the parser recorded; a `.pbs` carries no source text, so the
descriptor door keeps the file name alone. The driver drives
`data/escape-field.proto` and asserts a real `escape-field.proto:N:M:` prefix
next to both occupant names, and asserts that the descriptor run does not
fabricate a position.

## The three things every row needs

1. **Both input paths.** The source path (`.proto`) and the descriptor path
   (`.pbs`) must produce the **same header** for the same schema. The two
   frontends carry the same names, so the emitter has to spell them the same
   way. The driver compares the two headers byte for byte (the source half
   runs only in a parser-on profile).
2. **The header compiles.** A generated header that does not compile is a
   failure; "the tool exited 0" is not a pass. The build compiles every
   descriptor-path header into the runtime checker with the tree's own
   compiler and standard (`-std=c++17` in the full profile, `-std=c++11` in
   the C++11 profile), and the driver's byte-for-byte comparison extends
   that compile to the source path. The driver does not start the compiler
   itself: cl.exe outside the Visual Studio environment cannot find its own
   standard library headers (`C1083` on `<cstdint>`).
3. **The name carries the data.** An escaped member must still hold the value
   the schema names, and the value must still travel under the field number
   the schema gives it. The runtime checker writes every escaped member,
   compares the **bytes** on the wire, and reads them back: a member that was
   renamed but not wired to its field would compile cleanly and carry the
   wrong data.

The lane also drives the **refusal** through the CLI: a non-zero exit, an
empty stdout, and one diagnostic that names both occupants of the colliding
C++ name.

## The scope-occupant family

`family_cases.cmake` is the family as data, shared by the build and the
driver. It crosses the occupant kinds of a C++ scope — field, presence flag,
nested type, enumerator, generated codec, runtime namespace, the enclosing
class's own name, runtime type, global typedef — with the collision pairs that
can exist between them, both syntaxes and the three options that remove an
occupant (`--no-has-fields`, `--no-encoder`, `--no-decoder`).

- An **accepted** case must exit 0, leave stderr empty, emit a header, and
  produce the same bytes from the `.proto` and the `.pbs`. The build
  generates that exact header with the case's options and compiles it into
  `names_runtime.cpp`, so every accepted header in the family is compiled.
- A **refused** case must exit non-zero, leave stdout empty, and carry both
  occupants in its one diagnostic, on both input paths where source text
  exists.

The option matrix is the gating proof: an occupant an option removes must
stop occupying the name (for example `M::E::has_foo` without the flag of
`foo` under `--no-has-fields`), and an occupant no option removes must refuse
under every option set. The keyword-package refusal (`E5`) is deliberately
not in the family: it is an escape-rule divergence, not a scope collision.

`codegen.names.insertion_stem_collision` is the accepted `CR-44` xfail. It
asserts that `A_B` and `A::B` still share the four `EASYPB_A_B_*` macros, so
it fails when the insertion format becomes injective; the failure message
says which decision to re-evaluate.

## The fixtures

| Fixture | Path | Row |
| --- | --- | --- |
| a field named after a keyword | `data/keyword-field.proto` / `.pbs` | `W-08` |
| a message named after a keyword | `data/keyword-message.proto` / `.pbs` | `W-08` |
| an enum named after a keyword | `data/keyword-enum.proto` / `.pbs` | `W-08` |
| an enum value named after a keyword | `data/keyword-enum-value.proto` / `.pbs` | `W-08` |
| a nested message and a nested enum named after keywords | `data/keyword-nested.proto` / `.pbs` | `W-08` |
| a field named `has_foo` next to a field named `foo` (proto2) | `data/has-collision.proto` / `.pbs` | `W-09` |
| the same shape in proto3 | `data/has-collision-p3.proto` / `.pbs` | `W-09` |
| a nested message named `has_foo` next to a field named `foo` | `data/nested-flag.proto` / `.pbs` | `PC-01` |
| the fields `class` and `class_` | `data/escape-field.proto` / `.pbs` | `PC-02` |
| the messages `class` and `class_` | `data/escape-message.proto` / `.pbs` | `PC-02` |
| the enum values `class` and `class_` | `data/escape-enum-value.proto` / `.pbs` | `PC-02` |
| the enum `class` next to the message `class_` | `data/escape-enum-message.proto` / `.pbs` | `PC-02` |
| an enumerator against a presence flag (proto3, proto2) | `data/enum-flag-p3.proto`, `enum-flag-p2.proto` / `.pbs` | `CR-03` |
| an enumerator against a field (proto3, proto2) | `data/enum-field-p3.proto`, `enum-field-p2.proto` / `.pbs` | `CR-03` |
| two enumerators of two nested enums (proto3, proto2) | `data/enum-enum-p3.proto`, `enum-enum-p2.proto` / `.pbs` | `CR-03` |
| an enumerator against a nested type | `data/nested-enum-p3.proto` / `.pbs` | `CR-03` |
| a top-level message named `encode` (proto3, proto2) | `data/codec-message-encode.proto`, `codec-message-encode-p2.proto` / `.pbs` | `CR-04` |
| a top-level message named `decode` | `data/codec-message-decode.proto` / `.pbs` | `CR-04` |
| a top-level enumerator named `encode` / `decode` | `data/codec-enum-value-encode.proto`, `codec-enum-value-decode.proto` / `.pbs` | `CR-04` |
| top-level types named `easypb` / `std`, and an enum named `easypb` | `data/namespace-easypb.proto`, `namespace-std.proto`, `namespace-enum-easypb.proto` / `.pbs` | `CR-09` |
| the same two names inside a package (accepted control) | `data/namespace-packaged.proto` / `.pbs` | `CR-09` |
| a descriptor-origin field / enumerator / message / enum name that starts with a digit | `data/digit-{field,value,message,enum}.txtpb` / `.pbs` | `CR-10` |
| a nested message / nested enum / enumerator named like its message | `data/same-name-message.proto`, `same-name-enum.proto`, `same-name-enum-value.proto` / `.pbs` | `U-01` |
| `package easypb;` with a type named after each runtime type (`Encoder`, `Decoder`, `Writer`, `Reader`, `WireType`, `PBType`, `string_view`) | `data/runtime-{encoder,decoder,writer,reader,wiretype,pbtype,stringview}.proto` / `.pbs` | `U-02` |
| `package easypb;` with a non-colliding type, and one whose name differs by case (accepted controls) | `data/package-easypb.proto`, `package-easypb-case.proto` / `.pbs` | `U-02` |
| a top-level `int32_t` / `uint64_t` / `size_t` / `max_align_t` message | `data/global-{int32,uint64,size-t,max-align-t}.proto` / `.pbs` | `U-03` |
| `A_B` next to `A::B` (accepted) | `data/insertion-stem-collision.proto` / `.pbs` | `CR-44` |

Every fixture exists twice: as `.proto` source text and as the `.pbs`
descriptor of the same schema. FIVE descriptors are crafted, because no
`.proto` text can produce them. The four `digit-*` fixtures declare a
descriptor-origin name that is not a C++ identifier; the source lexer cannot
spell a digit-first name, so protoc cannot compile a `.proto` into that
descriptor, although it accepts such a set. `escape-enum-value` declares the
two enumerators `class` and `class_`, and the reference compiler refuses that
schema outright ("Enum name class_ has the same name as class if you ignore
case and strip out the enum name prefix"), so protoc cannot produce it either.
Those five `.pbs` files are encoded from the `.txtpb` beside each one with
`protoc --encode`. Every other `.pbs` is protoc's own encoding of the
`.proto` beside it - including the `U-01`, `U-02` and `U-03` rows, whose
schemas the reference compiler accepts.

## Rebuilding the fixtures

```sh
python tests/codegen/names/data/make_descriptors.py [--protoc PATH] [--check]
```

The script has no fixture table: it compiles every `.proto` here and encodes
every `.txtpb`, and the shared mechanics - the protoc lookup, the
`descriptor.proto` lookup, the text-format encoder and the writer - live in
`tests/codegen/common/descriptor_fixtures.py`. It writes the `.pbs` files with
`--descriptor_set_out`, or with `--encode` for the five crafted ones; a
`.txtpb` wins over the `.proto` of the same stem. `--check` rebuilds every
fixture in memory and fails on any byte that differs from the committed file.
Running the lane never needs `protoc`: the `.pbs` files are committed.

## The lanes it joins

| Test | Where |
| --- | --- |
| `codegen.names` | this lane: generate, run, refuse; the build compiles the headers |
| `codegen.names.insertion_stem_collision` | the accepted `CR-44` xfail |
| `codegen.packages.naming` | the unit rows of the name rules themselves |
| `codegen.imports.runtime` | the source path's header against the descriptor path's |

The lane runs in every build profile: the full C++17 tree, the full C++11
tree, the lite (parser-off) tree, and the legacy toolchains.

The driver also accepts `REPO_DIR` and `DESCRIPTOR_ROOT`. With both, it runs
the source path on the repository's own `codegen/easypb/options.proto` — the
`package easypb;` control that owner decision #1 keeps compiling — using the
same two roots the custom-types lane uses (the `codegen` directory that holds
`easypb/options.proto`, and the test-only `google/protobuf/descriptor.proto`).
The run succeeds with the one documented `extend` notice, which the driver
asserts, plus a `namespace easypb` and a `CppFieldOptions` declaration.
