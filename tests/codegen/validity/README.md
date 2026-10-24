# The validity lane - the emitter and the schema acceptance boundary

The lane began with the five rows of plan-5 follow-up session S1 (`W-02`,
`W-03`, `W-05`, `W-10`, `W-12`) and now also carries the acceptance boundary
of session S7a (`W-04`, `W-06`, `W-07`, `W-15`, `W-38`, `W-39`, `W-40`,
`W-41`, `W-42`, `W-43`). Every row is a counter-example against `protoc` 30.1,
and the lane proves the fix in the three ways the session briefs name.

## The three things every row needs

1. **Both input paths.** The source path (`.proto`) and the descriptor path
   (`.pbs`) must produce the **same header** for the same schema. The two
   frontends store a default value the same way - a string default decoded, a
   bytes default C-escaped, a floating default canonicalized - so the emitter
   has to read both the same way. The driver compares the two headers byte for
   byte (the source half runs only in a parser-on profile).
2. **The header compiles.** A generated header that does not compile is a
   failure; "the tool exited 0" is not a pass. The build compiles every
   descriptor-path header into the runtime checker with the tree's own
   compiler and standard (`-std=c++17` in the full profile, `-std=c++11` in
   the C++11 profile), and the driver's byte-for-byte comparison extends
   that compile to the source path. The driver does not start the compiler
   itself: cl.exe outside the Visual Studio environment cannot find its own
   standard library headers (`C1083` on `<cstdint>`).
3. **The value is right.** The runtime checker reads the values whose bytes
   are not visible in the tool's exit code: a backslash that became a
   backspace, a NUL that truncated a string, `1e-400` that became `-0`, and a
   finite subnormal are all checked at runtime. The lane also drives every
   refusal through the CLI: a non-zero exit, an empty stdout, and a diagnostic
   that names the offending thing.

## The fixtures

| Fixture | Paths | Row |
| --- | --- | --- |
| a quote in a string default | `.proto` + `.pbs` | `W-02` |
| a backslash in a string default | `.proto` + `.pbs` | `W-02` |
| a newline in a string default | `.proto` + `.pbs` | `W-02` |
| an embedded NUL (string and bytes) | `.proto` + `.pbs` | `W-02` |
| a quote in a bytes default | `.proto` + `.pbs` | `W-02` |
| a trigraph opener in a default | `.proto` + `.pbs` | `W-02` |
| `inf` and `-inf` | `.proto` + `.pbs` | `W-03` |
| `nan` | `.proto` + `.pbs` | `W-03` |
| an enum default that is not an identifier | crafted `.pbs` | `W-05` |
| a field whose type is outside the `TYPE_*` set | crafted `.pbs` | `W-10` |
| a field that declares no type | crafted `.pbs` | `W-10` |
| a group field | crafted `.pbs` | `W-10` |
| a logical name with a newline | crafted `.pbs` | `W-12` |
| the ERANGE float family (`1e400`, `1e-400`, `-1e-400`, `1e-320`) | `.proto` + raw `.pbs` | `W-04` |
| numeric default spellings (`010`, `0x10`, `-0`, 64-bit extremes) | `.proto` + `.pbs` | `CR-01`/`05`/`17` |
| descriptor float keywords (`INF`, `Infinity`, `NAN`, `nan(0x1)`) | crafted `.pbs` | `CR-02` |
| an out-of-range integer default | `.proto` + crafted `.pbs` | `W-06` |
| a default with no declared type | crafted `.pbs` | `W-07` |
| an empty `syntax = "";` statement | `.proto` only | `W-15` |
| an empty descriptor `syntax` string | crafted `.pbs` only | `W-15` |
| 101 package components (accepted) | `.proto` + crafted `.pbs` | `W-38` |
| 102 package components (refused) | `.proto` + crafted `.pbs` | `W-38` |
| a bytes default spelled `\X41` | `.proto` + raw `.pbs` | `W-39` |
| a leading UTF-8 BOM | `.proto` only | `W-41` |
| `FOO`/`foo` folded enum names | `.proto` + crafted `.pbs` | `W-42` |
| `E_FOO`/`FOO` folded enum names | `.proto` + crafted `.pbs` | `W-42` |
| a same-number alias (accepted) | `.proto` + `.pbs` | `W-42` |
| an aliased pair with different numbers (refused) | `.proto` + crafted `.pbs` | `W-42` |
| a proto3 default-JSON-name collision | `.proto` + crafted `.pbs` | `W-43` |
| two custom JSON names that collide | `.proto` + crafted `.pbs` | `W-43` |
| a custom-vs-default name in proto2 (warning) | `.proto` + crafted `.pbs` | `W-43` |
| a custom-vs-default name in proto3 (refused) | `.proto` + crafted `.pbs` | `W-43` |
| a map field custom JSON-name collision (refused) | `.proto` only | `W-43` |
| a proto2 default-JSON-name collision (warning) | `.proto` + crafted `.pbs` | `W-43` |
| distinct custom JSON names | `.proto` + `.pbs` | `W-43` |
| `+7`, `+1.0`, `+inf`, `+nan`, `A = +0` (refused) | `.proto` only | `W-40` |
| accepted `7`, `1.0`, `inf`, `nan`, `A = 0` controls | `.proto` + `.pbs` | `W-40` |
| a descriptor integer `+7` and float `+1.0` | crafted `.pbs` only | `W-40` |
| descriptor `-7` and `-1.0` controls | crafted `.pbs` only | `W-40` |
| a descriptor float default `" inf"` (refused) | crafted `.pbs` | `W-03` |
| source `[default =  inf]` and a string `" x"` (accepted) | `.proto` + `.pbs` | `W-03` |

The first eight paired fixtures and the accepted controls exist twice as
`.proto` source text and as their descriptor. The raw fixtures are crafted
because protoc canonicalizes or refuses the spelling they carry; the
`.txtpb` beside each is the readable form `make_descriptors.py` encodes.

## Path-restricted rows

- `W-15`: a descriptor cannot carry a `syntax = "";` statement; an empty
  descriptor `syntax` field is proto2 in `protoc` and stays accepted.
- `W-41`: a BOM is a property of source text. A `.pbs` is binary, and
  stripping bytes in the shared disk reader would corrupt it, so only the
  lexer consumes the BOM.
- `W-07`: a source field always declares a type (or a deferred type name), so
  the absent-type shape is descriptor-only; `W-10`'s decision is the rule that
  refuses it.

## The default-value family (B3)

`defaults-spellings` and `defaults-float-keywords` are the family test for the
default pipeline (`R1`: `CR-01`, `CR-02`, `CR-05`, `CR-17`). The driver runs
the matrix **spelling × field type (`float`, `double`, `int32`, `int64`,
`uint64`) × input path (`source`, `.pbs`) × option set (default,
`--no-has-fields`)** and asserts three things per cell:

1. the source and descriptor headers are byte-identical;
2. the emitted literal is the canonical one (`8` for `010`, `16` for `0x10`,
   `(-9223372036854775807LL - 1)` for the `int64` minimum,
   `18446744073709551615ULL` for the `uint64` maximum, `numeric_limits` for
   the CR-02 spellings) - this fails before a compiler could notice;
3. the runtime checker reads the decoded values as numbers, which is the
   value comparison against protoc's `--descriptor_set_out` (the `.pbs` is
   protoc's own encoding of the same source).

The `--no-has-fields` headers are generated by the build and compiled by
`codegen_validity_pedantic_check` under `-std=c++11 -pedantic-errors`. The
byte-for-byte comparison extends that compile to the source headers, so both
paths and both option sets are inside the P3 proof. The compile is a build
target rather than a driver command for the `cl.exe` reason above; a build
that succeeds proves it.

`defaults-float-keywords` is descriptor-only because `protoc`'s source grammar
is case-sensitive: a source `[default = INF]` is refused as an identifier,
while a descriptor carrying `INF` is accepted. The fixture carries the four
spellings the reference compiler normalizes plus the negative form.

## Rebuilding the fixtures

```sh
python tests/codegen/validity/data/make_descriptors.py [--protoc PATH]
```

The script needs `protoc` (30.1 is the oracle this project measures against)
and `google/protobuf/descriptor.proto`; it looks the latter up in
`$EASYPB_DESCRIPTOR_PROTO`, then in `build/protobuf-30.1-src/protobuf-30.1/src`,
then in the flat copy the repository already tracks for the differential lane.

The crafted fixtures come from `.txtpb` with
`--encode=google.protobuf.FileDescriptorSet`. A `.txtpb` wins over the
`.proto` of the same stem, because the raw spelling is the point. The
source-only fixtures (`bom`, `bom-control`, `empty-syntax`,
`json-name-map-collision`, `plus-int-default`, `plus-enum-value`,
`plus-float-default`, `plus-inf-nan`) are skipped: protoc refuses most of them,
and a generated `.pbs` would add an unintended twin.

**One byte is not protoc's text format.** `field-type-unknown` declares
`type: 99`, and the text format answers *"Unknown enumeration value of \"99\"
for field \"type\""*. The script therefore encodes that fixture twice - once
with `type: TYPE_INT32` and once with `type: TYPE_SINT32`, both one-byte
varints - asserts that the two encodings differ in exactly one byte, and
writes 99 into that byte. The result is protoc's own encoding of the same
descriptor with the unknown kind.

Running the lane never needs `protoc`: the `.pbs` files are committed.

## The lanes it joins

| Test | Where |
| --- | --- |
| `codegen.validity` | this lane: generate, run, refuse; the build compiles the headers |
| `codegen.imports.parity` | the descriptor path's verdicts against `protoc` |
| `codegen.imports.runtime` | the source path's header against the descriptor path's |
| `parser.unit` | the parser's stored text and its source-only refusals |

The lane runs in every build profile: the full C++17 tree, the full C++11
tree, the lite (parser-off) tree, and the legacy toolchains. The lite profile
skips the source halves and the source-only refusals; the descriptor halves
still run.
