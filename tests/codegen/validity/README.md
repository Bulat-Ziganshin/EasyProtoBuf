# The validity lane — the emitter's value and field-type layer

One lane for the five rows of the plan-5 follow-up's session S1: `W-02`,
`W-03`, `W-05`, `W-10`, `W-12`. Every row is a counter-example against
`protoc` 30.1, and the lane proves the fix in the three ways the session's
brief names.

## The three things every row needs

1. **Both input paths.** The source path (`.proto`) and the descriptor path
   (`.pbs`) must produce the **same header** for the same schema. The two
   frontends store a default value the same way — a string default decoded,
   a bytes default C-escaped — so the emitter has to read both the same way.
   The driver compares the two headers byte for byte (the source half runs
   only in a parser-on profile).
2. **The header compiles.** A generated header that does not compile is a
   failure; "the tool exited 0" is not a pass. The driver compiles every
   generated header with the tree's own compiler and the tree's own standard
   (`-std=c++17` in the full profile, `-std=c++11` in the C++11 profile), and
   the build compiles the same headers into the runtime checker.
3. **The value is right.** The runtime checker compares the **bytes** of
   every default, not its size. Two of the defects are silent: a backslash
   that became a backspace and a NUL that truncated the value both compile
   cleanly and are caught only here.

The lane also drives the **refusals** through the CLI: a non-zero exit, an
empty stdout, and a diagnostic that names the offending thing.

## The fixtures

| Fixture | Path | Row |
| --- | --- | --- |
| a quote in a string default | `data/defaults-quote.proto` / `.pbs` | `W-02` |
| a backslash in a string default | `data/defaults-backslash.proto` / `.pbs` | `W-02` |
| a newline in a string default | `data/defaults-newline.proto` / `.pbs` | `W-02` |
| an embedded NUL (string and bytes) | `data/defaults-nul.proto` / `.pbs` | `W-02` |
| a quote in a bytes default | `data/defaults-quote-byte.proto` / `.pbs` | `W-02` |
| a trigraph opener in a default | `data/defaults-trigraph.proto` / `.pbs` | `W-02` |
| `inf` and `-inf` | `data/defaults-inf.proto` / `.pbs` | `W-03` |
| `nan` | `data/defaults-nan.proto` / `.pbs` | `W-03` |
| an enum default that is not an identifier | `data/enum-default-identifier.pbs` | `W-05` |
| a field whose type is outside the `TYPE_*` set | `data/field-type-unknown.pbs` | `W-10` |
| a field that declares no type | `data/field-type-absent.pbs` | `W-10` |
| a group field | `data/field-type-group.pbs` | `W-10` |
| a logical name with a newline | `data/descriptor-name-newline.pbs` | `W-12` |

The seven paired fixtures exist twice: as `.proto` source text and as the
`.pbs` descriptor of the same schema. The five crafted fixtures are
descriptor-only, and their readable form is the `.txtpb` beside them.

## Rebuilding the fixtures

```sh
python tests/codegen/validity/data/make_descriptors.py [--protoc PATH]
```

The script needs `protoc` (30.1 is the oracle this project measures against)
and `google/protobuf/descriptor.proto`; it looks the latter up in
`$EASYPB_DESCRIPTOR_PROTO`, then in `build/protobuf-30.1-src/protobuf-30.1/src`,
then in the flat copy the repository already tracks for the differential lane.
It writes the seven paired `.pbs` files with `--descriptor_set_out` and the
five crafted ones with `--encode=google.protobuf.FileDescriptorSet`.

**One byte is not protoc's text format.** `field-type-unknown` declares
`type: 99`, and the text format answers *"Unknown enumeration value of \"99\"
for field \"type\""*. The script therefore encodes that fixture twice — once
with `type: TYPE_INT32` and once with `type: TYPE_SINT32`, both one-byte
varints — asserts that the two encodings differ in exactly one byte, and
writes 99 into that byte. The result is protoc's own encoding of the same
descriptor with the unknown kind.

Running the lane never needs `protoc`: the `.pbs` files are committed.

## The lanes it joins

| Test | Where |
| --- | --- |
| `codegen.validity` | this lane: generate, compile, run, refuse |
| `codegen.imports.parity` | the descriptor path's verdicts against `protoc` |
| `codegen.imports.runtime` | the source path's header against the descriptor path's |

The lane runs in every build profile: the full C++17 tree, the full C++11
tree, the lite (parser-off) tree, and the legacy toolchains.
