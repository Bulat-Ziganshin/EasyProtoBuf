# The library lane — a caller that links the core gets the CLI's answer

One lane for the four rows of the plan-5 follow-up's session `S6`:

- `W-11` — a default-constructed `GenerationOptions` produced
  `::std::vector a;` from `generate_header` and `generate_files`; the entry
  points now finalize a copy, so the defaults produce the CLI's header.
- `W-47` — compatibility mode tolerated an external **type** and with it a
  default its declared kind forbids (`::Missing f = x;`); the default is now
  checked for the declared kind while membership stays deferred to the
  dependency header.
- `W-48` — a hand-built descriptor with `has_name` / `has_package` false but
  the strings present lost its diagnostic label and indexed its package in the
  global scope; ingest now normalizes the two bits.
- `W-58` — `LinkedSchema::complete` meant both "the mode ran" and "the input
  was self-contained"; it is now `local_only`, one name for one meaning.

The shared theme is the API, not the executable: every row is a shape a caller
that links `easypb_codegen_core` can create without the CLI. The lane links
only the core (which brings `easypb_schema`), so it is parser-free and runs in
the lite profile.

Step `B8` added the `P7` family test to the same lane, because the
compatibility mode is the lane's subject: the thin, complete-closure and source
headers of one schema must be byte-identical (W-68), and the external enum
member they declare must be zero-initialized (CR-08). The thin header is
generated at build time so `thin_probe.cpp` compiles and runs against it
(`codegen.library.thin`); the script compares the three spellings
(`codegen.library`).

## The two halves

1. `test_library.cpp` — the C++ API suite: default-constructed options,
   hand-built descriptors with the presence bits cleared, an in-memory
   compatibility link, and the link result's `local_only` flag. Its
   `--emit-options-default <path>` mode writes the `W-11` header for the
   build-time compile check.
2. `test_library.cmake` — the CLI half: the `W-47` refusals and controls, the
   `W-47` source-path control, the library-vs-CLI byte identity for the same
   descriptor set, and the `P7` thin/full/source triple.

The `W-11` header is compiled by the build, not by the test script: `cl.exe`
started outside the Visual Studio environment cannot find its own standard
library headers (`C1083` on `<cstdint>`). The driver's byte-for-byte
comparison extends that compile to the CLI's header.

## The fixtures

| Fixture | Role |
| --- | --- |
| `data/options-default.proto` / `.pbs` | `W-11`: a repeated scalar and a map |
| `data/w47-external.proto` / `.pbs` | `W-47` control: an external message, no default |
| `data/w47-external-message-default.pbs` | `W-47`: a message field with a default (crafted) |
| `data/w47-external-enum.proto` / `w47-external-enum-default.pbs` | `W-47` control: an external enum with default `A` |
| `data/w47-external-enum-1a.pbs` | `W-47`: an external enum with default `1A` (crafted) |
| `data/w47-source-default.proto` | `W-47` source half: an unresolved type with a default |
| `data/w48-wire.proto` / `.pbs` | `W-48` wire control; its hand-built twin clears both bits |
| `data/p7-ext/a.proto`, `b.proto` / `p7-thin.pbs`, `p7-full.pbs` | `P7`/CR-08: a proto3 external enum; thin (no `--include_imports`) and complete closure |
| `thin_probe.cpp` | `P7`/CR-08 runtime probe: placement-new over `0xAB` reads the enum's zero default |

`w47-external-enum-1a.pbs` carries no dependency edge on purpose: the
`--target` control must reach type resolution (`unresolved type .dep.E`), not
stop at the missing import.

## Rebuilding the fixtures

```sh
python tests/codegen/library/data/make_descriptors.py [--protoc PATH]
```

The script needs `protoc` (30.1 is the oracle this project measures against)
and `google/protobuf/descriptor.proto`; it looks the latter up in
`$EASYPB_DESCRIPTOR_PROTO`, then in `build/protobuf-30.1-src/protobuf-30.1/src`,
then in the flat copy the repository already tracks for the differential lane.
It writes the accepted `.pbs` files with `--descriptor_set_out` and the crafted
ones with `--encode=google.protobuf.FileDescriptorSet`. Running the lane never
needs `protoc`: the `.pbs` files are committed.
