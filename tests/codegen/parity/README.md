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
| a map entry in key-then-value order (accepted) | `data/map-ok.proto` / `.pbs` | `W-37` |
| a map entry that declares the value first | `data/map-reversed.pbs` | `W-37` |
| a proto3 enum whose first value is zero (accepted) | `data/enum-ok.proto` / `.pbs` | `W-44` |
| a proto3 enum whose first value is non-zero | `data/enum-first.proto` / `.pbs` | `W-44` |
| a proto2 range that leaves room for its field (accepted) | `data/ext-ok.proto` / `.pbs` | `W-45` |
| a proto3 extension range | `data/ext-p3.proto` / `.pbs` | `W-45` |
| a range that includes a declared field | `data/ext-field.proto` / `.pbs` | `W-45` |
| two overlapping ranges | `data/ext-overlap.proto` / `.pbs` | `W-45` |
| a thin set: the import is named but not carried | `data/w68-root.proto` / `.pbs` | `W-68` |
| the imported file | `data/w68-dep.proto` / `.pbs` | `W-68` |

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
python tests/codegen/parity/data/make_descriptors.py [--protoc PATH]
```

The script needs `protoc` (30.1 is the oracle this project measures against)
and `google/protobuf/descriptor.proto`; it looks the latter up in
`$EASYPB_DESCRIPTOR_PROTO`, then in `build/protobuf-30.1-src/protobuf-30.1/src`,
then in the flat copy the repository already tracks for the differential lane.
It writes the accepted `.pbs` files with `--descriptor_set_out` and the crafted
ones with `--encode=google.protobuf.FileDescriptorSet`.

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
