# The validity lane - the emitter and the schema acceptance boundary

The lane began with the five rows of plan-5 follow-up session S1 (`W-02`,
`W-03`, `W-05`, `W-10`, `W-12`) and now also carries the acceptance boundary
of session S7a (`W-04`, `W-06`, `W-07`, `W-15`, `W-38`, `W-39`, `W-40`,
`W-41`, `W-42`, `W-43`). Every row is a counter-example against `protoc` 30.1,
and the lane proves the fix in the three ways the session briefs name.

The review of 2026-10-27 added two rows of the file-scope name rules
(`U-02`/`U-03`): the names lane owns the rule, and this lane proves the two
input paths reach the same verdict and the same diagnostic for it. The review
of 2026-10-28 extends that proof to the completed rules: the complete runtime
declaration list under `package easypb;` (`V-01`), the preamble's C library and
implementation names in the global namespace and the reserved [lex.name]
spelling of a top-level declaration (`V-02`), plus the accepted nested `_Foo`
that keeps its name.

Step R05 adds the accepted twins of the descriptor invariants the parity lane
refuses: an explicit proto3 `optional` field (one synthetic one-field oneof,
`U-11`), a oneof whose members are adjacent in the field list (`U-21`), and a
proto2 MessageSet with no ordinary fields (`U-10`/`U-12`). Each pair is
protoc-written and must emit the same header through both doors.

Step R06 adds the subnormal-float and reserved-boundary accepted pairs
(`U-18`, `U-56`) and the refusal twins of the nested-enum reserved rules, the
leading-space integral default and the NUL `json_name` (`U-14`, `U-29`,
`U-57`). The subnormal pair also carries the `--print-descriptor` parity row:
the default text is the same on both doors.

Step R07 adds the accepted lite-import pair (`U-15`, exception `E15`): a
non-lite file importing a `LITE_RUNTIME` file, accepted on both doors with the
same header and no diagnostic.

Step R08 adds the accepted option-default pair (`U-08`): `lazy = false`,
`unverified_lazy = false`, `jstype = JS_NORMAL` and `packed = false` on a map
must emit the same header as the option-less control on both doors.

Step R11 adds the `--no-class` pair (`U-20`): the embedded-NUL default that is
refused for a custom string type stays accepted when `--no-class` suppresses
the member initializer the refusal describes, while the same flag keeps a
schema-level field-number refusal. It also moves the U-03 refusal comparison to
the sentence alone: the source door now carries `file:line:column` (`U-30`) and
the descriptor door cannot, so the driver asserts the position on the source
path and its absence on the descriptor path instead of byte-equal stderr.

Step R14 adds the reserved-name warning pair (`U-50`) and the two `map_entry`
diagnostics (`U-62`), and extends the W-42 rows with the unconditional
`allow_alias` suffix (`U-51`). The warning fixture is source-only: the rule is
the parser's, and a descriptor carries no source text; `reserved "ok"` in the
same file is the silent control, while the header must still be written. The
two `map_entry` fixtures split the value-kind rule from the row's own refusal:
an aggregate (message-shaped) value names the kind defect, and a well-typed
`true` keeps the `map_entry` sentence. The W-42 suffix is asserted on both
doors and in both `allow_alias` branches.

Step S03 adds the map value-enum rule (`V-06`) and the descriptor-door
extension member rules (`V-09`): a proto2 map whose value enum does not start
at zero is refused on both doors with protoc's sentence, the zero-first control
is accepted, a legal extension member is accepted on both doors with the E6
warning, and a member whose tag is inside the protocol-reserved block is
refused with the source door's own sentence.

Step S04 adds the bool default spelling (`V-24`): the paired `bool-default`
fixture proves both doors emit `true` and `false` through the same header, and
the crafted `bool-default-invalid.pbs` - a spelling only a raw descriptor can
carry - proves the refusal. The emitter repeats the check on the text it
writes, so a caller that links the core and builds the schema itself is
refused there too (`codegen.library`).

Step S05 adds the print-mode half of the MessageSet rule (`V-25`):
`--print-descriptor` used to print a `message_set_wire_format` message that also
declares ordinary fields and exit 0, while the generation path refused it. The
row drives the canonical refused pair from the parity lane
(`../parity/data/message-set-fields.proto` and `.pbs`) through generation and
print on both doors: the descriptor-door diagnostics are compared byte for byte,
and the source-door pair must agree on the sentence and the declaration
position. The accepted control is the paired `message-set-empty` fixture, which
still prints with exit 0.

Step S06 adds the retained half of the map `packed = false` rule (`V-16`): the
print path writes `[packed=false]` only when `FieldOptions.has_packed` is set,
so the `option-value-defaults` fixture now also proves that the source model
keeps the explicit `false` and that the two doors print the same tree. The
generated headers already had to be equal (the R08 row above); this row pins
the model fact the header cannot show.

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
| the escape table on `string` and `bytes` | `.proto` + `.pbs` | `CR-12`/`13`/`29` |
| the leading-zero lexeme table | `.proto` + `.pbs` | `CR-18` |
| descriptor-carried `08`, `09`, `010.5`, `010` | crafted `.pbs` | `CR-18` |
| `08` in a float default, `09` in an option, `08` in an enum | `.proto` only | `CR-18` |
| a nested block comment | `.proto` only | `CR-30` |
| a raw NUL in an aggregate option | `.proto` only | `CR-19` |
| the accepted statement/option grammar table | `.proto` + `.pbs` | `CR-11`/`20`/`26`/`27`/`28` |
| a leading `;` before `syntax` (refused) | `.proto` only | `CR-20` |
| a repeated field with a default (refused) | `.proto` only | `CR-11` |
| `allow_alias = true` without a shared number (refused) | `.proto` + crafted `.pbs` | `CR-26` |
| `allow_alias = false` without a shared number (refused) | `.proto` + crafted `.pbs` | `CR-26` |
| `allow_alias = false` with a shared number (refused) | `.proto` + crafted `.pbs` | `CR-26` |
| `json_name` on an enum value (refused) | `.proto` only | `CR-27` |
| a leading `+` on ordinary options (refused) | `.proto` only | `CR-28` |
| `package easypb;` with a non-colliding type (accepted) | `.proto` + `.pbs` | `U-02` |
| a global-namespace `message int32_t` (refused on both paths) | `.proto` + `.pbs` | `U-03` |
| `package easypb;` with a declaration named `exception` (refused on both paths) | `.proto` + `.pbs` | `V-01` |
| a global-namespace `__gnu_cxx` / `memcpy` message (refused on both paths) | `.proto` + `.pbs` | `V-02` |
| a top-level `_Foo` message (refused on both paths) | `.proto` + `.pbs` | `V-02` |
| a package-local `_Foo` nested in a message (accepted) | `.proto` + `.pbs` | `V-02` |
| an explicit proto3 `optional` field (accepted) | `.proto` + `.pbs` | `U-11` |
| adjacent members of one oneof (accepted) | `.proto` + `.pbs` | `U-21` |
| a proto2 MessageSet with no fields (accepted) | `.proto` + `.pbs` | `U-10`/`U-12` |
| finite subnormal float defaults (`1e-40`, `1.4e-45`) | `.proto` + `.pbs` | `U-18` |
| a reserved declaration naming 536870912 (accepted) | `.proto` + `.pbs` | `U-56` |
| a reserved number above 536870912 and a range to 2147483646 (accepted) | `.proto` + `.pbs` | `V-03` |
| enum values named `true`/`false`/`inf`/`nan` used as defaults (accepted) | `.proto` + `.pbs` | `V-04` |
| MessageSet extension ranges above 536870911 (accepted) | `.proto` + `.pbs` | `V-05` |
| a proto2 map with a zero-first enum value (accepted) | `.proto` + `.pbs` | `V-06` |
| a proto2 map whose enum value type starts at 1 (refused) | `.proto` + crafted `.pbs` | `V-06` |
| an extension member with a protocol-reserved tag (refused on both paths) | `.proto` + crafted `.pbs` | `V-09` |
| a valid extension member (accepted on both paths, E6 warning) | `.proto` + `.pbs` | `V-09` |
| `true`/`false` bool defaults (accepted on both paths) | `.proto` + `.pbs` | `V-24` |
| a bool default spelled `yes` (refused) | crafted `.pbs` | `V-24` |
| a bare `;` inside a oneof (refused) | `.proto` only | `V-08` |
| a package-local proto3 `extend FileOptions` (accepted) | `.proto` only | `V-12` |
| an empty and a semicolon-only `extend` body (refused) | `.proto` only | `V-17` |
| an inverted reserved range in a message-nested enum | `.proto` + crafted `.pbs` | `U-14` |
| an enum value reusing a nested enum's reserved number | `.proto` + crafted `.pbs` | `U-14` |
| an integral default with a leading space (refused) | crafted `.pbs` | `U-29` |
| a field number of 536870912 (refused) | `.proto` only | `U-56` |
| a custom `json_name` with an embedded NUL | `.proto` + crafted `.pbs` | `U-57` |
| a non-lite file importing a `LITE_RUNTIME` file (accepted) | `.proto` + crafted `.pbs` | `U-15` |
| the four option default values, with an option-less control (accepted) | `.proto` + `.pbs` | `U-08`, `V-16` |
| an extension `json_name` that differs from the derived name (refused) | `.proto` only | `U-25` |
| an extension `json_name` equal to the derived name (accepted) | `.proto` only | `U-25` |
| a reserved name that is not an identifier (accepted, warning) | `.proto` only | `U-50` |
| `map_entry` with a message-shaped value (refused: value kind) | `.proto` only | `U-62` |
| `map_entry` with a well-typed primitive value (refused: `map_entry`) | `.proto` only | `U-62` |
| the embedded-NUL default under `--no-class` and a custom string type (accepted) | `defaults-nul.proto` / `.pbs` | `U-20` |

The first eight paired fixtures and the accepted controls exist twice as
`.proto` source text and as their descriptor. The raw fixtures are crafted
because protoc canonicalizes or refuses the spelling they carry; the
`.txtpb` beside each is the readable form `make_descriptors.py` encodes.

## The lite-import exception (R07)

`nonlite-imports-lite` is the accepted pair for `U-15` (exception `E15`): a
non-lite file importing a file that sets `optimize_for = LITE_RUNTIME`. The
reference compiler refuses it - a file that does not use lite semantics cannot
import one that does; EasyProtoBuf has one runtime, so the option is validated
and ignored. The driver asserts that both doors accept the pair, print no
diagnostic, and emit the same header byte for byte, dependency include
included.

The descriptor half is crafted from `nonlite-imports-lite.txtpb` because
protoc will not compile the source text it mirrors; `lite-runtime-dep.proto`
is source-only for the generator for the same reason - its descriptor exists
only inside that set.

## Path-restricted rows

- `W-15`: a descriptor cannot carry a `syntax = "";` statement; an empty
  descriptor `syntax` field is proto2 in `protoc` and stays accepted.
- `W-41`: a BOM is a property of source text. A `.pbs` is binary, and
  stripping bytes in the shared disk reader would corrupt it, so only the
  lexer consumes the BOM.
- `W-07`: a source field always declares a type (or a deferred type name), so
  the absent-type shape is descriptor-only; `W-10`'s decision is the rule that
  refuses it.
- `U-25`: the extension `json_name` rule is a source-text option rule. The
  descriptor door decodes extension members since `V-09`, but it validates
  their extendee/tag/MessageSet shape, not their options, so both the refusal
  and the equal-spelling control exist only as `.proto` text.
- `V-08`, `V-17`: a bare `;` inside a oneof and an empty or semicolon-only
  `extend` body are statement-grammar shapes. protoc refuses them, so it cannot
  write a descriptor, and a `.pbs` has no statement grammar to carry them; the
  source door owns both refusals.
- `V-12`: the proto3 `extend` allowlist compares the resolved extendee name
  (`ValidateProto3Field`, `descriptor.cc:7907-7911`). The descriptor door
  decodes extension members since `V-09`, but the allowlist remains a
  source-text rule there, so a `.pbs` cannot witness it; the fixture uses an
  imported stand-in for `google.protobuf.FileOptions` and is driven through the
  source door only.

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

## The lexer and literal family (B4)

`literals-escapes` and `literals-lexemes` are the family test for the tokenizer
and the string decoder (`R2`: `CR-12`, `CR-13`, `CR-18`, `CR-19`, `CR-29`,
`CR-30`). The driver runs the matrix **escape rule × literal kind × input path**
plus the **lexeme table** and asserts four things per cell:

1. the source and descriptor headers are byte-identical;
2. the emitted C++ literal is the decoded value (`"\377"` for `\777`,
   `"\360\237\230\200"` for `\ud83d\ude00`, `"\355\240\200"` for a lone
   `\ud800`, `"x\ry"` for a raw CR, `"\\U00110000"` for a code point above
   `0x10FFFF`);
3. the runtime checker reads the bytes as values, which is the comparison
   against `protoc`'s `--descriptor_set_out` (the `.pbs` is protoc's own
   encoding of the same source);
4. every refusal is driven through the CLI: non-zero exit, empty stdout, and a
   diagnostic that names the rule.

The escape table is one row per `protoc` rule (`io/tokenizer.cc`:
`ConsumeString`, `ParseStringAppend`, `FetchUnicodePoint`, `AppendUTF8`):
`\x`/`\X` with one or two digits, octal with one to three digits truncating to
the low byte, `\u` with four digits, `\U` bounded to `00[01]xxxxxx`, a UTF-16
head+trail pair, a lone surrogate (three bytes), a head without a trail, a code
point above `0x10FFFF` (the escape text comes back verbatim), and a raw CR.
Each row exists for `string` and, where the value is a byte sequence, for
`bytes`; the `.pbs` half proves the descriptor path stores the same bytes.

The lexeme table is the leading-zero branch of `ConsumeNumber`: `0x` is hex, a
leading zero makes the token octal, and an octal token has no 8/9 digit, no
fraction and no exponent. `literals-octal-08`, `literals-octal-09` and
`literals-octal-enum` cover the float default, the ordinary option value and
the enum value; `literals-descriptor-octal` is a crafted set because `protoc`
refuses to write a descriptor from the source it refuses, and it pins the
descriptor path's own rule (a float/double spelling `08`, `09` or `010.5` is
accepted, an integral `08` is refused - both match `protoc
--descriptor_set_in`). `literals-nested-comment` and `literals-aggregate-nul`
are the two structural refusals; both are source-only because neither shape can
travel in a descriptor.

The family is not a set of refusals only: `literals-lexemes` also carries
`LEXEME_OCTAL = 010` and `reserved 050 to 060`, the non-default consumers of
the same token rule, and the runtime checker reads the enumerator as 8.

The lite profile skips the source halves of every row above; the descriptor
halves still run.

## The statement and option family (B7)

`grammar-table` is the family test for the source frontend's statement and
option grammar (`R5`: `CR-11`, `CR-20`, `CR-26`, `CR-27`, `CR-28`). It carries
one accepted spelling per rule, and the paired loop compares the source header
with protoc's own `.pbs` byte for byte:

- `CR-11` — a oneof member with an explicit default (the repeated-field
  refusal is the invalid twin, `grammar-repeated-default`);
- `CR-20` — an empty statement after `syntax` is legal; a leading one is not;
- `CR-26` — `allow_alias = true` with a shared number;
- `CR-27` — an enum value without field options;
- `CR-28` — an ordinary option value without a leading `+`.

The invalid spellings are `grammar-leading-semicolon`, `grammar-repeated-default`,
`grammar-allow-alias-useless`, `grammar-allow-alias-false`,
`grammar-allow-alias-false-aliased`, `grammar-enum-json-name`,
`grammar-option-plus`, `grammar-option-plus-known` and
`grammar-field-option-plus`. All nine are source-only: protoc refuses the text,
so it cannot write a descriptor for them (an enum value's `EnumValueOptions` has
no `json_name` field at all).

`CR-26` has a descriptor half because its two rules live in protoc's *parser*
(`ValidateEnum`, `parser.cc:520-575`), not in its descriptor builder. A
descriptor carrying `allow_alias = true` without a shared number, or an
explicit `false`, is therefore accepted by `protoc --descriptor_set_in`, and
the crafted `grammar-allow-alias-*-descriptor.pbs` companions pin that verdict:
codegen accepts the first two and refuses the third by the ordinary
duplicate-number rule, exactly as protoc's descriptor path does. The crafted
bytes come from protoc's own text-format encoder because source text cannot
produce the shape.

The family's accepted headers are compiled by
`codegen_validity_pedantic_check` under `-std=c++11 -pedantic-errors` (the
`grammar-table` descriptor header); the source header is byte-identical, so the
compile covers both paths.

## Rebuilding the fixtures

```sh
python tests/codegen/validity/data/make_descriptors.py [--protoc PATH] [--check]
```

The source-only list, the patched fixture and the patch itself are in that
script; the protoc lookup, the `descriptor.proto` lookup, the text-format
encoder and the writer are shared with the other descriptor lanes in
`tests/codegen/common/descriptor_fixtures.py`.
The crafted fixtures come from `.txtpb` with
`--encode=google.protobuf.FileDescriptorSet`. A `.txtpb` wins over the
`.proto` of the same stem, because the raw spelling is the point. The
source-only fixtures (`bom`, `bom-control`, `empty-syntax`,
`ext-json-name`, `ext-json-name-equal`, `extend-empty`, `extend-semicolon`,
`grammar-allow-alias-false`, `grammar-allow-alias-false-aliased`,
`grammar-allow-alias-useless`, `grammar-enum-json-name`,
`grammar-field-option-plus`, `grammar-leading-semicolon`,
`grammar-option-plus`, `grammar-option-plus-known`,
`grammar-repeated-default`, `json-name-map-collision`,
`lite-runtime-dep`,
`literals-aggregate-nul`, `literals-nested-comment`, `literals-octal-08`,
`literals-octal-09`, `literals-octal-enum`, `map-entry-message-value`,
`map-entry-primitive-value`, `oneof-empty-statement`,
`package-extend-options`, `plus-int-default`,
`plus-enum-value`, `plus-float-default`, `plus-inf-nan`,
`proto3-package-extend`, `reserved-non-identifier`)
are skipped: protoc refuses most of them,
and a generated `.pbs` would add an unintended twin. `lite-runtime-dep` is
skipped because protoc refuses its importer, so the pair is encoded from
`nonlite-imports-lite.txtpb` and the dependency has no standalone descriptor.
`package-extend-options` is the stand-in extendee `proto3-package-extend`
imports; both are source-only because neither shape travels in a descriptor.
`ext-json-name*` are skipped for the opposite reason: the descriptor model
does not decode extension members (E6), so only the source half carries the
`U-25` rule.

**One byte is not protoc's text format.** `field-type-unknown` declares
`type: 99`, and the text format answers *"Unknown enumeration value of \"99\"
for field \"type\""*. The script therefore encodes that fixture twice - once
with `type: TYPE_INT32` and once with `type: TYPE_SINT32`, both one-byte
varints - asserts that the two encodings differ in exactly one byte, and
writes 99 into that byte. The result is protoc's own encoding of the same
descriptor with the unknown kind.

`--check` rebuilds every fixture in memory and fails on any byte that differs
from the committed file.

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
