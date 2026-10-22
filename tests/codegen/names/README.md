# The names lane — the emitter's name layer

One lane for the two rows of the plan-5 follow-up's session S2: `W-08` (a
C++ keyword in a declaration name) and `W-09` (a presence flag that collides
with a member of the same message), and for the post-closure review of
2026-10-22 that found both rules incomplete: a nested type collides with a
presence flag as well (`PC-01`), and the escape rule is not injective, so two
Protobuf names can be one C++ name (`PC-02`). Every row is a counter-example
against `protoc` 30.1, and the lane proves the fix in the three ways the
session's brief names.

## The three things every row needs

1. **Both input paths.** The source path (`.proto`) and the descriptor path
   (`.pbs`) must produce the **same header** for the same schema. The two
   frontends carry the same names, so the emitter has to spell them the same
   way. The driver compares the two headers byte for byte (the source half
   runs only in a parser-on profile).
2. **The header compiles.** A generated header that does not compile is a
   failure; "the tool exited 0" is not a pass. The driver compiles every
   generated header with the tree's own compiler and the tree's own standard
   (`-std=c++17` in the full profile, `-std=c++11` in the C++11 profile), and
   the build compiles the same headers into the runtime checker.
3. **The name carries the data.** An escaped member must still hold the value
   the schema names, and the value must still travel under the field number
   the schema gives it. The runtime checker writes every escaped member,
   compares the **bytes** on the wire, and reads them back: a member that was
   renamed but not wired to its field would compile cleanly and carry the
   wrong data.

The lane also drives the **refusal** through the CLI: a non-zero exit, an
empty stdout, and a diagnostic that names the collision.

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

Every fixture exists twice: as `.proto` source text and as the `.pbs`
descriptor of the same schema. ONE descriptor is crafted rather than
compiled: `escape-enum-value` declares the two enumerators `class` and
`class_`, and the reference compiler refuses that schema outright ("Enum
name class_ has the same name as class if you ignore case and strip out the
enum name prefix"), so protoc cannot produce it — its `.pbs` is encoded from
`data/escape-enum-value.txtpb` with `protoc --encode`. Every other `.pbs` is
protoc's own encoding of the `.proto` beside it.

## Rebuilding the fixtures

```sh
python tests/codegen/names/data/make_descriptors.py [--protoc PATH]
```

The script needs `protoc` (30.1 is the oracle this project measures against)
and writes the `.pbs` files with `--descriptor_set_out`.

Running the lane never needs `protoc`: the `.pbs` files are committed.

## The lanes it joins

| Test | Where |
| --- | --- |
| `codegen.names` | this lane: generate, compile, run, refuse |
| `codegen.packages.naming` | the unit rows of the name rules themselves |
| `codegen.imports.runtime` | the source path's header against the descriptor path's |

The lane runs in every build profile: the full C++17 tree, the full C++11
tree, the lite (parser-off) tree, and the legacy toolchains.
