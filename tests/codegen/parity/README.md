# The parity lane — one rule, two input paths

One lane for the six rows of the plan-5 follow-up's session `S5`: `W-16`
(method endpoints never resolved; duplicate method names checked only on the
descriptor path), `W-36` (two descriptor sets holding one logical file are
rejected wholesale), `W-37` (the map entry is located by field number, not
declaration order), `W-44` (a proto3 enum whose first value is non-zero is
accepted on the descriptor path), `W-45` (extension ranges are only counted on
the descriptor path) and `W-68` (the compatibility header drops its import
includes). The shared defect behind all six is the same: a rule existed on one
frontend, or the two frontends read one descriptor differently, so the same
schema was accepted by one path and refused by the other.

Step B5 extends the lane with the descriptor-model root `R3`: `CR-06`
(message/enum `reserved` ranges and names), `CR-07` (the extension-range upper
bound), `CR-23` (a MessageSet that declares fields) and `CR-46` (the three
optional declaration names), plus the accepted `CR-25` xfail. The same rule
applies: every shape is a verdict on both doors, and the pair of accepted
controls (`ext-max`, `message-set-empty`) is compared byte for byte.

Step R04 (`U-13`, `U-22`) adds the descriptor option rows: the two field
options whose rules change a verdict (`jstype`, `lazy`/`unverified_lazy`) and
the extension range's `verification`/`declaration` pair. They are the same
kind of row - a rule that used to exist on the source door only, now asked on
both - with the accepted controls compared byte for byte.

Step R05 (`U-10`, `U-11`, `U-12`, `U-21`) adds the descriptor invariants around
presence, oneofs and MessageSet. `message_set_wire_format` is the one option the
source parser used to validate and drop; it now travels in the model, so a
MessageSet with fields (`U-10`) and a proto3 MessageSet (`U-12`) are verdicts on
both doors. The `proto3_optional` rules (`U-11`) and the oneof-contiguity rule
(`U-21`) are descriptor-only by construction - the source parser synthesizes the
flag's one-field oneof and parses oneof bodies as one block - and their accepted
twins live in the validity lane.

## The three things every row needs

1. **Both input paths.** The source path (`.proto`) and the descriptor path
   (`.pbs`) must agree. For every schema both paths accept, the driver
   compares the two headers **byte for byte** — a rule that lives on one path
   only shows up here as a difference, even when both exits are zero. The
   source half runs only in a parser-on profile; `FULL_BUILD=0` skips it and
   the descriptor half still runs.
2. **The header compiles.** A generated header that does not compile is a
   failure; "the tool exited 0" is not a pass. The build's compile check
   target compiles every accepted descriptor-path header with the tree's own
   compiler and standard (`-std=c++17` in the full profile, `-std=c++11` in
   the C++11 profile), and the driver's byte-for-byte comparison extends
   that compile to the source path. The driver does not start the compiler
   itself: cl.exe outside the Visual Studio environment cannot find its own
   standard library headers (`C1083` on `<cstdint>`).
3. **The refusal is the same on both paths.** Every shape the source path
   refuses, the descriptor path refuses too — and the other way round — with
   a non-zero exit, an empty stdout and a diagnostic that names the offending
   thing.

## The fixtures

| Fixture | Path | Row |
| --- | --- | --- |
| service methods with real endpoints (accepted) | `data/methods-ok.proto` / `.pbs` | `W-16` |
| a duplicate method name | `data/methods-dup.proto` / `.pbs` | `W-16` |
| a method endpoint that names nothing | `data/methods-missing.proto` / `.pbs` | `W-16` |
| one logical file in two identical containers | `data/w36-same-{a,b}.pbs` | `W-36` |
| one logical file in two conflicting containers | `data/w36-conflict-{a,b}.pbs` | `W-36` |
| a proto3 packed default beside `packed = true` (accepted) | `data/w36-packed-default.pbs`, `data/w36-packed-true.pbs` | `U-04` |
| a proto3 packed default beside `packed = false` (refused) | `data/w36-packed-default.pbs`, `data/w36-packed-unpacked.pbs` | `U-04` |
| a proto2 packed default beside `packed = false` (accepted) | `data/w36-packed-proto2-{default,false}.pbs` | `U-04` |
| a proto2 packed default beside `packed = true` (refused) | `data/w36-packed-proto2-{default,true}.pbs` | `U-04` |
| an unpackable repeated string beside `packed = false` (accepted) | `data/w36-packed-string-{default,false}.pbs` | `U-04` |
| an absent label beside `LABEL_OPTIONAL` (accepted) | `data/w36-label-{absent,explicit}.pbs` | `U-07` |
| `LABEL_OPTIONAL` beside `LABEL_REQUIRED` (refused) | `data/w36-label-{explicit,required}.pbs` | `U-07` |
| an empty enum reserved range beside `[0, 0]` (accepted) | `data/w36-enum-reserved-{empty,zero}.pbs` | `U-07` |
| an enum reserved range `[0, 0]` beside `[2, 2]` (refused) | `data/w36-enum-reserved-{zero,two}.pbs` | `U-07` |
| an int32 default `0x0A` beside `10` (accepted) | `data/w36-default-{hex,decimal}.pbs` | `U-07` |
| an int32 default `10` beside `11` (refused) | `data/w36-default-{decimal,other}.pbs` | `U-07` |
| an unsupported class carried by `edition` beside one carried by `syntax` (refused) | `data/w36-edition-{empty,absent}.pbs` | `U-52` |
| a map entry in key-then-value order (accepted) | `data/map-ok.proto` / `.pbs` | `W-37` |
| a map entry that declares the value first | `data/map-reversed.pbs` | `W-37` |
| a proto3 enum whose first value is zero (accepted) | `data/enum-ok.proto` / `.pbs` | `W-44` |
| a proto3 enum whose first value is non-zero | `data/enum-first.proto` / `.pbs` | `W-44` |
| a proto2 range that leaves room for its field (accepted) | `data/ext-ok.proto` / `.pbs` | `W-45` |
| a proto3 extension range | `data/ext-p3.proto` / `.pbs` | `W-45` |
| a range that includes a declared field | `data/ext-field.proto` / `.pbs` | `W-45` |
| two overlapping ranges | `data/ext-overlap.proto` / `.pbs` | `W-45` |
| the accepted extension-range boundary (end = 536870912) | `data/ext-max.proto` / `.pbs` | `CR-07` |
| an extension range above the protocol maximum | `data/ext-high.proto` / `.pbs` | `CR-07` |
| a field number inside a reserved range | `data/res-field-number.proto` / `.pbs` | `CR-06` |
| a field name that is reserved | `data/res-field-name.proto` / `.pbs` | `CR-06` |
| an extension range overlapping a reserved range | `data/res-ext-overlap.proto` / `.pbs` | `CR-06` |
| the same reserved range twice | `data/res-dup-range.proto` / `.pbs` | `CR-06` |
| the same reserved name twice | `data/res-dup-name.proto` / `.pbs` | `CR-06` |
| an enum value using a reserved number | `data/enum-reserved.proto` / `.pbs` | `CR-06` |
| an enum value using a reserved name | `data/enum-reserved-name.proto` / `.pbs` | `CR-06` |
| a MessageSet with no fields (accepted) | `data/message-set-empty.proto` / `.pbs` | `CR-23`, `U-10`/`U-12` |
| reserved ranges/names and an extension range (accepted) | `data/retained-ranges.proto` / `.pbs` | `V-15` |
| all three field labels (accepted) | `data/labels-ok.proto` / `.pbs` | `V-42` |
| a MessageSet that declares a field | `data/message-set-fields.proto` / `.pbs` | `CR-23`, `U-10` |
| a MessageSet in a proto3 file | `data/message-set-proto3.proto` / `.pbs` | `U-12` |
| `proto3_optional` with no oneof | `data/p3opt-no-oneof.pbs` | `U-11` |
| `proto3_optional` in a proto2 file | `data/p3opt-proto2.pbs` | `U-11` |
| `proto3_optional` in a two-field oneof | `data/p3opt-two-field-oneof.pbs` | `U-11` |
| `proto3_optional` on a repeated field | `data/p3opt-repeated.pbs` | `U-11` |
| a synthetic oneof before a real oneof | `data/p3opt-synthetic-first.pbs` | `U-11` |
| members of one oneof separated by an ordinary field | `data/oneof-not-contiguous.pbs` | `U-21` |
| an unnamed field / message / service | `data/no-{field,message,service}-name.pbs` | `CR-46` |
| a `jstype` on a 64-bit field (accepted) | `data/opt-jstype-int64.proto` / `.pbs` | `U-13` |
| a `jstype` on an int32 field | `data/opt-jstype-int32.proto` / `.pbs` | `U-13` |
| an undeclared `jstype` enumerator (accepted, reads as absent) | `data/opt-jstype-unknown.pbs` | `U-13` |
| `lazy = true` on a message field (accepted) | `data/opt-lazy-message.proto` / `.pbs` | `U-13` |
| `lazy = true` on a scalar | `data/opt-lazy-int32.proto` / `.pbs` | `U-13` |
| `unverified_lazy = true` on a scalar | `data/opt-unverified-lazy-int32.proto` / `.pbs` | `U-13` |
| an extension declaration list (accepted) | `data/opt-extrange-declaration.proto` / `.pbs` | `U-22` |
| the same declaration `full_name` in two ranges | `data/opt-extrange-decl-name-duplicate.pbs` | `V-07` |
| a declaration `full_name` without the leading dot | `data/opt-extrange-decl-name-no-dot.pbs` | `V-07` |
| a declaration `full_name` with an invalid identifier | `data/opt-extrange-decl-name-invalid.pbs` | `V-07` |
| a declaration `type` with an invalid identifier | `data/opt-extrange-decl-type-invalid.pbs` | `V-07` |
| `UNVERIFIED` beside a declaration | `data/opt-extrange-unverified.proto` / `.pbs` | `U-22` |
| a declaration number outside the range | `data/opt-extrange-decl-bad-number.pbs` | `U-22` |
| one number declared twice | `data/opt-extrange-decl-duplicate.pbs` | `U-22` |
| a declaration with only `full_name` | `data/opt-extrange-decl-partial.pbs` | `U-22` |
| an extension member with no extendee | `data/ext-member-no-extendee.pbs` | `V-09` |
| an extension member outside the declared range | `data/ext-member-out-of-range.pbs` | `V-09` |
| the source `extend` of an unknown type (accepted, E6) | `data/ext-unvalidated.proto` | `CR-25`, E6 |
| a thin set: the import is named but not carried | `data/w68-root.proto` / `.pbs` | `W-68` |
| the imported file | `data/w68-dep.proto` / `.pbs` | `W-68` |
| an undeclared `edition` enumerator (accepted, reads as absent) | `data/descriptor-wire-edition-unknown.pbs` | `V-26` |
| 100 nested unknown groups (accepted) | `data/descriptor-wire-groups-100.pbs` | `V-28` |
| 101 nested unknown groups | `data/descriptor-wire-groups-101.pbs` | `V-28` |
| a mid-message tag with field number 0 | `data/descriptor-wire-tag-zero.pbs` | `V-41` |
| the accepted wire control | `data/descriptor-wire-base.pbs` | S08 |

The merge rows (`U-04`, `U-07`, `U-52`) are descriptor-only by construction:
the source path reads one definition of a file, so only two containers can
disagree about a spelling. Each accepted pair is driven through both argument
orders and must produce the same header as either single container; each
refused pair must answer "two containers carry different definitions of one
logical file" in both orders. The `.proto` fixture cannot carry the pair, so
the driver does not run a source half for these rows.

The S08 wire rows (`descriptor-wire-*`) are RAW fixtures: they are not a
`.proto`/`.txtpb` pair but bytes written by `common/descriptor_wire.py`, because
the text format validates a closed enum and has no unknown fields. All five
declare the same logical file, so the accepted three must generate one header
and print one tree; the undeclared `edition` enumerator must be absent from the
print (protoc routes it to the unknown fields), and the 101-level nest and the
zero tag are refused on both the generation and the print door.

Two shapes cannot be written as `.proto` source at all, and the driver says so
where it skips the source half:

- **a reversed map entry** (`W-37`) — the source parser synthesises the entry
  in key-then-value order, so a source file cannot carry the malformed
  declaration; only a hand-built descriptor can;
- **two containers holding one logical file** (`W-36`) — the source path reads
  a file tree, which has one definition of a file by construction.

`data/ext-overlap.proto` exists although `protoc` refuses to compile it: the
source path is this project's own parser, and the refusal is part of the lane.
The same is true of `message-set-fields.proto` and `message-set-proto3.proto`,
whose refusals are real rows now that the source door models the option. The
descriptor halves of the shapes `protoc` refuses are encoded from the `.txtpb`
beside them, so the readable form of every crafted descriptor is in the tree.

The step-B5 rows (`CR-06`, `CR-07`, `CR-23`, `CR-46`) follow the same rule.
Each reserved/extension shape has a `.proto` half the source parser answers as
text and a `.pbs` half encoded from the `.txtpb` beside it, because `protoc`
refuses the shape and cannot write its descriptor. `ext-max` and
`message-set-empty` are the accepted controls: `protoc` accepts them, so their
`.pbs` halves are protoc-written and the accepted-pair check compares the two
headers byte for byte.

Step S06 closes the source half of `V-15`: the parser now stores the `reserved`
ranges and names and the extension ranges it validated, so
`--print-descriptor` reads one model on both doors. `retained-ranges` is the
accepted pair for that rule; its two dumps must be the same bytes, and the
driver names each declaration the source dump used to drop. The pair is also in
the accepted-header loop, so the model change is proven not to affect the
generated header.

Step S07 adds `labels-ok` (`V-42`): a proto2 message with `optional`,
`required` and `repeated` fields, so the accepted side of the label rule is one
verdict on both doors and the descriptor header is compiled by the lane's
compile check. The out-of-range value (99) has no source spelling at all:
protoc routes it to the unknown fields, this project refuses it at ingestion,
and the descriptor-only row that proves the refusal lives in the validity lane
(`data/label-unknown.pbs`).

Two B5 shapes were descriptor-only when the rule was written:

- **a MessageSet that declares fields** (`CR-23`) — the source parser used to
  parse a message option as an uninterpreted value and discard it (B1 gap `G2`).
  R05 (`U-10`) closes that gap: the option travels in the model, so the
  field-bearing shape is refused on both doors now, and the field-less control is
  still accepted on both with an unchanged header.
- **an unnamed field/message/service** (`CR-46`) — the source grammar cannot
  spell a nameless declaration; only a crafted descriptor can.

The four `p3opt-*` rows and `oneof-not-contiguous` are descriptor-only by
construction (`U-11`, `U-21`): the source parser sets `proto3_optional` only for
an explicit proto3 `optional`, synthesizes the one-field oneof itself and appends
it after every real oneof, and parses a oneof body as one block. Their accepted
twins - `p3opt-ok`, `oneof-contiguous` and `message-set-empty` - are in the
validity lane, which compares the two headers byte for byte.

The R04 rows (`U-13`, `U-22`) follow the same rule. The `jstype` and either
lazy flag are decoded by the descriptor door and validated by the shared
semantic pass, so the refused shapes exist on both doors with the same
sentence, and the accepted controls (`opt-jstype-int64`, `opt-lazy-message`)
are protoc-written pairs whose headers must be byte-identical. One accepted
descriptor carries a byte the text format cannot spell: `opt-jstype-unknown`
writes an enumerator that the pinned `descriptor.proto` does not declare,
which protoc's closed-enum decoder routes to the unknown fields, so the schema
is accepted; the `codegen.schema.semantics` unit test pins that the decoded
option is absent rather than kept.

`opt-extrange-declaration` is the accepted `U-22` control, and it is the one
pair whose descriptor half is crafted although protoc accepts the source:
`descriptor.proto` marks both `declaration` and `verification`
`[retention = RETENTION_SOURCE]`, so a protoc-written descriptor carries
neither option. The source half is an ordinary `.proto` protoc accepts, and the
lane still compares the two headers byte for byte.

Three `ExtensionRangeOptions` shapes are descriptor-only:

- **a declaration number outside its range, the same number twice, and a
  declaration with only one of `full_name`/`type`** (`opt-extrange-decl-*`) —
  the source door keeps an aggregate option body opaque (P1 exception `E13`,
  and the `codegen.options` lane pins the accepted aggregate spelling), so the
  source half does not model the members. `opt-extrange-unverified` is the one
  option conflict both doors model: the parser refuses it as text and the
  descriptor sweep refuses the decoded pair.

The `V-07` declaration clauses are descriptor-only by construction: `U-22`'s
accepted control already explains why the source door keeps the aggregate
option body opaque (E13), so protoc's four extra declaration checks run only on
the decoded list. The accepted control stays `opt-extrange-declaration`.

`V-09` moved the descriptor door's extension members out of the accepted
register. The decoder reads `DescriptorProto.extension` and
`FileDescriptorProto.extension`, and the link validates each member: a missing
extendee and a tag outside the extendee's declared range are refused with
protoc's sentence, an extension of a MessageSet is an optional message, and a
valid set records the E6 warning. The source door still parses an `extend` body
as text and discards it, so `ext-unvalidated.proto` (an unknown extendee)
remains the accepted `CR-25`/E6 row. The separate test
`codegen.parity.extensions_validated` asserts both halves: the two descriptor
refusals and the source acceptance.

## The documented divergences

Compatibility mode (`--descriptor-set` without `--target`) is deliberately
more permissive than the source path for an **absolute** reference into a file
the set does not carry: a thin single-file descriptor cannot know whether the
name is external, so it warns (the A5 external-type warning) instead of
refusing. The lane pins both sides:

- `data/methods-missing.pbs` alone is accepted with the warning, and refused
  with `--target methods-missing.proto` — exactly as the source path refuses
  `methods-missing.proto`;
- `data/w68-root.pbs` alone is accepted with the warning, and refused with
  `--target w68-root.proto` because the import is genuinely absent from the
  set.

The divergence is a property of the **mode**, not of the path: `--target` puts
the descriptor path into the complete mode the source path always runs in, and
there the two agree. It is recorded in the follow-up ledger's Decisions table.

The one thing compatibility mode still emits from the thin set is the include
of the absent import — that is `W-68`: the header must name the dependency's
header even when only the target is carried, because a translation unit that
includes it needs the name. `W-68`'s header is byte-identical to the one the
complete closure produces.

## Rebuilding the fixtures

```sh
python tests/codegen/parity/data/make_descriptors.py [--protoc PATH] [--check]
```

The fixture table is in that script; the protoc lookup, the
`descriptor.proto` lookup, the text-format encoder and the writer are shared
with the other descriptor lanes in
`tests/codegen/common/descriptor_fixtures.py`. It writes the accepted `.pbs`
files with `--descriptor_set_out` and the crafted ones with
`--encode=google.protobuf.FileDescriptorSet`. `--check` rebuilds every fixture
in memory and fails on any byte that differs from the committed file.

Running the lane never needs `protoc`: the `.pbs` files are committed.

## The lanes it joins

| Test | Where |
| --- | --- |
| `codegen.parity` | this lane: one rule, two input paths |
| `codegen.imports.parity` | the descriptor path's verdicts against `protoc` |
| `codegen.imports.runtime` | the source path's header against the descriptor path's |
| `codegen.validity`, `codegen.names`, `codegen.limits` | the earlier session lanes, the same three claims |

The lane runs in every build profile: the full C++17 tree, the full C++11
tree, the lite (parser-off) tree, and the legacy toolchains.
