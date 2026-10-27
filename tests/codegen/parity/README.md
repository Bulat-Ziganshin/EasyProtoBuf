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
| a MessageSet with no fields (accepted) | `data/message-set-empty.proto` / `.pbs` | `CR-23` |
| a MessageSet that declares a field | `data/message-set-fields.pbs` | `CR-23` |
| an unnamed field / message / service | `data/no-{field,message,service}-name.pbs` | `CR-46` |
| a `jstype` on a 64-bit field (accepted) | `data/opt-jstype-int64.proto` / `.pbs` | `U-13` |
| a `jstype` on an int32 field | `data/opt-jstype-int32.proto` / `.pbs` | `U-13` |
| an undeclared `jstype` enumerator (accepted, reads as absent) | `data/opt-jstype-unknown.pbs` | `U-13` |
| `lazy = true` on a message field (accepted) | `data/opt-lazy-message.proto` / `.pbs` | `U-13` |
| `lazy = true` on a scalar | `data/opt-lazy-int32.proto` / `.pbs` | `U-13` |
| `unverified_lazy = true` on a scalar | `data/opt-unverified-lazy-int32.proto` / `.pbs` | `U-13` |
| an extension declaration list (accepted) | `data/opt-extrange-declaration.proto` / `.pbs` | `U-22` |
| `UNVERIFIED` beside a declaration | `data/opt-extrange-unverified.proto` / `.pbs` | `U-22` |
| a declaration number outside the range | `data/opt-extrange-decl-bad-number.pbs` | `U-22` |
| one number declared twice | `data/opt-extrange-decl-duplicate.pbs` | `U-22` |
| a declaration with only `full_name` | `data/opt-extrange-decl-partial.pbs` | `U-22` |
| extension declarations are not validated (xfail) | `data/ext-member-*.pbs`, `data/ext-unvalidated.proto` | `CR-25`, E6 |
| a thin set: the import is named but not carried | `data/w68-root.proto` / `.pbs` | `W-68` |
| the imported file | `data/w68-dep.proto` / `.pbs` | `W-68` |

The merge rows (`U-04`, `U-07`, `U-52`) are descriptor-only by construction:
the source path reads one definition of a file, so only two containers can
disagree about a spelling. Each accepted pair is driven through both argument
orders and must produce the same header as either single container; each
refused pair must answer "two containers carry different definitions of one
logical file" in both orders. The `.proto` fixture cannot carry the pair, so
the driver does not run a source half for these rows.

Two shapes cannot be written as `.proto` source at all, and the driver says so
where it skips the source half:

- **a reversed map entry** (`W-37`) — the source parser synthesises the entry
  in key-then-value order, so a source file cannot carry the malformed
  declaration; only a hand-built descriptor can;
- **two containers holding one logical file** (`W-36`) — the source path reads
  a file tree, which has one definition of a file by construction.

`data/ext-overlap.proto` exists although `protoc` refuses to compile it: the
source path is this project's own parser, and the refusal is part of the lane.
The descriptor halves of the shapes `protoc` refuses are encoded from the
`.txtpb` beside them, so the readable form of every crafted descriptor is in
the tree.

The step-B5 rows (`CR-06`, `CR-07`, `CR-23`, `CR-46`) follow the same rule.
Each reserved/extension shape has a `.proto` half the source parser answers as
text and a `.pbs` half encoded from the `.txtpb` beside it, because `protoc`
refuses the shape and cannot write its descriptor. `ext-max` and
`message-set-empty` are the accepted controls: `protoc` accepts them, so their
`.pbs` halves are protoc-written and the accepted-pair check compares the two
headers byte for byte.

Two B5 shapes are descriptor-only by construction:

- **a MessageSet that declares fields** (`CR-23`) — the source parser parses a
  message option as an uninterpreted value and discards it (B1 gap `G2`), so
  the field-bearing shape cannot reach the model from source text. The
  field-less control is accepted on both paths and its header is unchanged.
- **an unnamed field/message/service** (`CR-46`) — the source grammar cannot
  spell a nameless declaration; only a crafted descriptor can.

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

The accepted `CR-25` decision is the lane's one xfail, and it is a separate
test: `codegen.parity.extensions_not_validated` asserts that an extension
member with no extendee, one outside the declared range, and a source
`extend` of an unknown type are still accepted. It passes while the accepted
decision stands and fails when extensions become validated, which is the
re-evaluation trigger in `triage.md`.

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
