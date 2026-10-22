# CLI-lane fixtures (S4)

The files here are the fixtures of the plan-5 follow-up session S4 (the
command-line contract). They are read by
[`../test_codegen_cli.cmake`](../test_codegen_cli.cmake); the lane passes this
directory as `FIXTURE_DIR`.

| File | Shape | Row |
| --- | --- | --- |
| `hasfield.proto` | proto2 message with one optional scalar field | `W-20`, `W-21`, `W-22` |
| `extend.proto` | proto2 message plus an `extend` block the trimmed model drops | `W-14` |
| `anon.pbs` | one file with no logical name and no external reference | `W-18` |
| `external-anon.pbs` | one unnamed file whose field is an unresolved external message type; the compatibility link records one warning | `W-14`, `W-18` |
| `casepair.pbs` | `api/x.proto` (message `Lower`) and `api/X.proto` (message `Upper`) | `W-26` |

The three `.pbs` files are crafted, because `protoc` cannot produce an unnamed
`FileDescriptorProto` and a case-only pair is a deliberate shape. They are
rebuilt by:

```sh
python tests/codegen/cli/data/make_fixtures.py
```

The script is deterministic; the committed bytes are its output. The `.proto`
files are ordinary source fixtures and are edited by hand.

The non-ASCII fixtures of `W-24` are not committed: the lane writes
`схема.proto`, `каталог/plain.proto` and a copy of
`tests/codegen/parser/data/integration-proto2.pbs` named `схема.pbs` at run
time, below its own work directory, so the test does not depend on a committed
file name that a checkout or archive tool might normalize.
