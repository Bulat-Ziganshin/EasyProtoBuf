# The `codegen.options` lane

Black-box coverage for D2-A: the source parser validates known options against
the closed `protoc` 30.1 table (`codegen/parser/known_options.hpp`), while
custom options stay opaque. The table-driven unit test in `parser.unit`
covers all 47 rows in three directions (accepted kind, wrong kind, wrong
owner); this lane proves the same verdicts through the executable, including
the five measured `G2` cases and the parser-vs-semantic split for `lazy`.

The lane is source-driven and the source half is skipped when the build has no
`.proto` parser (`FULL_BUILD=0`). It never calls `protoc`: every fixture is
committed and the CI contract is the recorded verdict. The one descriptor half
(`feature-set-*`, step S13) runs in every profile because a `.pbs` needs no
parser.

## Accepted fixtures

| Fixture | What it pins |
| --- | --- |
| `accept-file-options.proto` | `FileOptions` strings, bools and `OptimizeMode` |
| `accept-message-options.proto` | `MessageOptions` bools, including `message_set_wire_format` on a field-less MessageSet |
| `accept-field-options.proto` | `FieldOptions`: 64-bit `jstype`, message `lazy`/`unverified_lazy`, `ctype`, `weak`, `packed` both ways, `retention`, repeated `targets`, aggregate `feature_support`/`edition_defaults` |
| `accept-value-defaults.proto` | the four `U-08` default spellings: `lazy = false` and `unverified_lazy = false` on an `int32`, `jstype = JS_NORMAL` on an `int32`, `packed = false` on a map |
| `accept-cpp-type-spellings.proto` | `(easypb.cpp).type` on a field in its three spellings: plain, fully-qualified leading dot and spaced parentheses; the header check reads the three customized members |
| `accept-enum-options.proto` | `EnumOptions.allow_alias` and enum-value bool/aggregate options |
| `accept-service-options.proto` | `ServiceOptions.deprecated`, `MethodOptions.deprecated`/`idempotency_level` |
| `accept-ext-range-options.proto` | repeated aggregate `declaration`, and `verification = UNVERIFIED` on a range without declarations |
| `accept-custom-options.proto` | parenthesized custom names at all nine owner scopes; the values are opaque (policy, see below) |
| `accept-proto3-extend-options.proto` | a proto3 `extend` for each of the nine `google.protobuf.*Options` extendees, plus one leading-dot spelling; the bodies are discarded with the E6 warning (`U-16`) |
| `accept-aggregate-bodies.proto` | the three aggregate-valued standard options with an unknown inner field; the bodies are brace-balanced only (policy, see below) |

The driver also reads the generated header of `accept-field-options.proto` and
checks that `packed = true` and `packed = false` still select the packed and
unpacked codecs. An option that is accepted but silently changes the emitter
would otherwise pass this lane.

## The V-20 opaque-option pin (S13)

`FileOptions` is one of the option messages the descriptor decoder does not
model: `FileDescriptorProto.options` is skipped as a length-delimited field, so
neither its own fields nor a nested `FeatureSet` are decoded. Three descriptor
fixtures pin the boundary (owner decision D-04):

| Fixture | What it pins |
| --- | --- |
| `feature-set-control.pbs` | the option-less schema: one file, one message, one `int32` field |
| `feature-set-opaque.pbs` | the same file with a well-formed `FeatureSet` in `FileOptions` (`field_presence: LEGACY_REQUIRED`, `enum_type: CLOSED`, `repeated_field_encoding: EXPANDED`, `utf8_validation: NONE`, `message_encoding: DELIMITED`) |
| `feature-set-opaque-malformed.pbs` | the same file whose `features` payload is not a decodable `FeatureSet` (an invalid inner tag); protoc refuses it, this decoder skips the whole `FileOptions` frame |

All three declare `feature-set-opaque.proto`, so all three must generate the
same header byte for byte. `data/make_descriptors.py` builds the two textual
halves from `.txtpb` and the malformed half from raw bytes; `--check` is the
byte-identity gate.

## Policy: the value decides

The field rules of D2-A are value rules (`U-08`). `lazy` and `unverified_lazy`
are refused only as `true` on a non-message field, `jstype` only as a value
other than `JS_NORMAL` on a non-64-bit field, and `packed` only as `true` on a
field that is not packable. The four default spellings are therefore accepted
on the same entity kinds where their non-default twin is refused:
`accept-value-defaults.proto` pins `lazy = false`, `unverified_lazy = false`,
`jstype = JS_NORMAL` (on an `int32`) and `packed = false` (on a map), while
the `reject-*` rows pin the non-default values. The emitter proof - the four
defaults emit the option-less header on both doors - is the `codegen.validity`
row.

## Refused fixtures

Each `reject-*.proto` is one refusal. The driver requires a nonzero exit, empty
stdout, and the expected sentence in stderr:

| Fixture | Expected diagnostic |
| --- | --- |
| `reject-file-string-kind.proto` | quoted string for string option |
| `reject-file-bool-kind.proto` | identifier for boolean option |
| `reject-optimize-for-kind.proto` | identifier for enum-valued option |
| `reject-optimize-for-value.proto` | no enum value named `TURBO` |
| `reject-jstype-kind.proto` | `jstype is only allowed` |
| `reject-jstype-value.proto` | no enum value named `JS_TEXT` |
| `reject-lazy-scalar.proto` | submessage fields |
| `reject-unverified-lazy-scalar.proto` | submessage fields |
| `reject-lazy-enum.proto` | submessage fields, after the named type resolves to an enum |
| `reject-cpp-type-file.proto` | `(easypb.cpp).type` is a field option: file scope |
| `reject-cpp-type-message.proto` | the same option at message scope |
| `reject-cpp-type-enum.proto` | the same option at enum scope |
| `reject-cpp-type-ext-range.proto` | the same option at extension range scope |
| `reject-cpp-prefix-file.proto` | `(easypb.cppfoo)` is unknown, not a field option (`V-29`) |
| `reject-cpp-prefix-message.proto` | the same non-family name at message scope |
| `reject-proto3-extend-user.proto` | a proto3 `extend` of a user message: `extend declarations are allowed only in proto2` (`U-16`) |
| `reject-map-entry.proto` | `map_entry should not be set explicitly` |
| `reject-map-entry-kind.proto` | a non-boolean `map_entry` value still names the explicit-setting rule first (`V-30`) |
| `reject-map-entry-string.proto` | the string value takes the same explicit-setting sentence |
| `reject-map-entry-aggregate.proto` | the aggregate value takes the same explicit-setting sentence |
| `reject-map-packed.proto` | `map fields cannot have default or packed options` (the non-default `packed = true`) |
| `reject-ext-range-unverified-declaration.proto` | `UNVERIFIED` with declarations |
| `reject-file-unknown-option.proto` | `Option "java_packages" unknown` |
| `reject-file-option-placement.proto` | file option inside a message |
| `reject-field-option-on-enum.proto` | field option on an enum |
| `reject-field-bracket-placement.proto` | file option in a field bracket list |
| `reject-oneof-option.proto` | `OneofOptions` has no known field |
| `reject-method-option-on-service.proto` | method option on a service |
| `reject-ext-range-option-placement.proto` | range option on a message |
| `reject-enum-value-option-placement.proto` | field option on an enum value |
| `reject-file-option-duplicate.proto` | singular option set twice |
| `reject-field-option-duplicate.proto` | singular field option set twice |
| `reject-custom-leading-identifier.proto` | an ordinary leading name part is unknown |
| `reject-aggregate-unterminated.proto` | `unterminated aggregate option value` |

## Policy: unresolved custom options stay opaque

`protoc` resolves a parenthesized option against an imported extension
definition and refuses the file when the definition is absent. This frontend
does not link options, so it validates the name grammar and balances the value
but deliberately accepts an unresolved custom option. That is an owner
decision from D2-A (2026-10-25), recorded as P1 exception **E13** in
`build/verification/property-catalogue.md`; the executable side of the
exception is `accept-custom-options.proto` in this lane. `(easypb.cpp)` is the
one custom name with product semantics: it is a **field** option (`U-54`), so
its `.type` member is accepted in a field bracket list and refused at every
other declaration scope with a diagnostic that names the option and the scope
(`reject-cpp-type-*`). The header check on `accept-cpp-type-spellings.proto`
keeps the three accepted field spellings tied to the emitted members. The
family is matched by its full name (`V-29`): the bare `(easypb.cpp)` and its
`.`-members are the family, while a longer name in EasyProtoBuf's own namespace
such as `(easypb.cppfoo)` is an unknown option (`reject-cpp-prefix-*`) rather
than an opaque third-party custom option.

The table validates top-level option names only. A dotted sub-field assignment
to a singular message-typed option (`[feature_support.edition_introduced =
...]`) is protoc-valid but refused here as an unknown name; the aggregate
spelling `[feature_support = { edition_introduced: ... }]` is the supported
form. This stricter boundary is P1 exception **E14**, pinned by
`policy-feature-support-subfield.proto`.

## Proto3 `extend` declarations (`U-16`)

`accept-proto3-extend-options.proto` declares an extension for each of the nine
`google.protobuf.*Options` messages and repeats one with the leading-dot
spelling. That is the complete proto3 whitelist of protoc's
`AllowedExtendeeInProto3` (`descriptor.cc:1105-1131`); the parser compares the
extendee name and does not resolve the import, so the declaration is accepted
without linking `descriptor.proto`. The bodies are validated and discarded, and
the run prints the E6 warning `extend declaration parsed but not stored by the
trimmed FileDescriptorProto model`; the header is the ordinary one for the
file's message. `reject-proto3-extend-user.proto` is the other direction: a
proto3 `extend` of a user message keeps the proto2-only refusal.

The `google/protobuf/descriptor.proto` beside the fixtures is a minimal
stand-in: the nine option messages with an extension range, enough to make the
fixture protoc-valid without copying the real descriptor. The parser does not
read it; the loader only has to find the import the fixture names.

The known table is closed at `protoc` 30.1. An option added by a later release
is unknown until the table is refreshed; the doc comment in
`known_options.cpp` names the source file and the exclusion rule
(`uninterpreted_option` and the nine `features` fields).

## Policy: aggregate option bodies are brace-balanced only

`declaration`, `edition_defaults` and `feature_support` are the three
aggregate-valued fields of the table. A body gets the same deliberate opacity
as a parenthesized custom value (E13): the tokenizer runs and the braces are
counted, but no field of the inner message is looked up. `[declaration =
{ bogus: 5 }]` is therefore accepted where `protoc` refuses the unknown field.
This is the owner decision of 2026-10-27 for `U-09` (variant Б), recorded in
`codegen/OPTIONS.md` ("Aggregate option bodies are brace-balanced only"):
`accept-aggregate-bodies.proto` pins the acceptance half and
`reject-aggregate-unterminated.proto` pins the balance half.

## The SD-1 xfail: `deprecated_legacy_json_field_conflicts`

| Test | What it pins |
| --- | --- |
| `codegen.options.legacy_json_xfail` | the option is accepted on both owners (message and enum), and the two legacy behaviours it would enable - the bracketed `json_name` and the proto2 folded enum-name collision - stay refused |

`descriptor.proto` marks the field on `MessageOptions` (11) and `EnumOptions`
(6) `[deprecated = true]`. Stage decision SD-1 (2026-10-27) keeps both rows in
the known table and makes the option a documented no-op: a migration leftover
is accepted, nothing reads the value, and no warning is added. The driver
re-drives the same three verdicts the stage decision names, so the option
cannot be silently dropped again; if either refused shape starts to pass, the
test fails and `codegen/OPTIONS.md` has to be updated before the assertion
changes.

## Running

```powershell
ctest --test-dir build/verification/tree-cxx17 -R '^codegen[.]options$' --output-on-failure
```
