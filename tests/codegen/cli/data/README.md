# CLI-lane fixtures (S4, S7c)

The files here are the fixtures of the plan-5 follow-up sessions S4 (the
command-line contract) and S7c (the command line's own surface). They are read
by [`../test_codegen_cli.cmake`](../test_codegen_cli.cmake); the lane passes
this directory as `FIXTURE_DIR`.

| File | Shape | Row |
| --- | --- | --- |
| `hasfield.proto` | proto2 message with one optional scalar field | `W-20`, `W-21`, `W-22`, `W-29` |
| `extend.proto` | proto2 message plus an `extend` block the trimmed model drops | `W-14` |
| `anon.pbs` | one file with no logical name and no external reference | `W-18`, `W-19` |
| `external-anon.pbs` | one unnamed file whose field is an unresolved external message type; the compatibility link records one warning | `W-14`, `W-18` |
| `casepair.pbs` | `api/x.proto` (message `Lower`) and `api/X.proto` (message `Upper`) | `W-26` |
| `legacy.protodevel` | proto3 file with a `.protodevel` suffix and one scalar field | `W-25` |
| `legacy.protodevel.pbs` | `protoc`'s descriptor set of `legacy.protodevel`, so both input paths carry the same file | `W-25` |
| `cycle.proto` | two messages whose fields are each other by value (a value cycle) | `W-66` |
| `cycle.pbs` | `protoc`'s descriptor set of `cycle.proto` | `W-66` |
| `bad-anon.pbs` | one unnamed file whose message declares field number 1 twice | `W-19` |
| `anon-two.pbs` | two unnamed files | `W-19` |
| `mixed-anon.pbs` | one named and one unnamed file | `W-19` |
| `defaults-empty.proto` | proto2 message with `optional bytes b = 1 [default = ""]` | `W-35` |
| `defaults-empty.pbs` | `protoc`'s descriptor set of `defaults-empty.proto`, so both input paths carry the empty default | `W-35` |

`bad-anon.pbs`, `anon-two.pbs`, `mixed-anon.pbs` and the three original `.pbs`
files are crafted, because `protoc` cannot produce an unnamed
`FileDescriptorProto` and a case-only pair is a deliberate shape. They are
rebuilt by:

```sh
python tests/codegen/cli/data/make_fixtures.py
```

The script is deterministic; the committed bytes are its output. The `.proto`
files are ordinary source fixtures and are edited by hand.
`legacy.protodevel.pbs` and `cycle.pbs` are ordinary `protoc` 30.1 output:

```sh
cd tests/codegen/cli/data
protoc -I . --descriptor_set_out=legacy.protodevel.pbs legacy.protodevel
protoc -I . --descriptor_set_out=cycle.pbs cycle.proto
protoc -I . --descriptor_set_out=defaults-empty.pbs defaults-empty.proto
```

The non-ASCII fixtures of `W-24` are not committed: the lane writes
`схема.proto`, `каталог/plain.proto` and a copy of
`tests/codegen/parser/data/integration-proto2.pbs` named `схема.pbs` at run
time, below its own work directory, so the test does not depend on a committed
file name that a checkout or archive tool might normalize.
