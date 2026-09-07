# Custom-type descriptor fixtures

The `.pbs` files are committed so the descriptor-input tests do not require an
installed `protoc`. Descriptor bytes may differ between `protoc` versions even
when Codegen produces identical C++; `.proto`/`.pbs` output parity is therefore
the semantic check used by the tests.

## Ordinary fixtures

`data/custom_types.pbs` and `data/template_cases.pbs` are generated from their
same-named `.proto` files. Use `data` and the repository's `codegen` directory
as include roots, for example:

```sh
protoc \
    -I tests/codegen/custom_types/data \
    -I codegen \
    -I /path/to/protobuf/include \
    --descriptor_set_out=tests/codegen/custom_types/data/custom_types.pbs \
    tests/codegen/custom_types/data/custom_types.proto
```

Do not add `--include_imports`: Codegen currently requires a descriptor set to
contain exactly one `FileDescriptorProto`.

## Descriptor-only edge cases

- `empty_cpp_type.pbs` contains an `int32 value` field whose
  `(easypb.cpp).type` member is present but is the empty string. It can be
  produced from a temporary schema importing `easypb/options.proto` and using
  `int32 value = 1 [(easypb.cpp).type = ""];`.
- `cpp_group_without_type.pbs` contains two repeated string fields. The first
  has a zero-length `CppFieldOptions` payload; the second has only an unknown
  future member, field number 2 with the varint value `true`. It was produced
  with a temporary copy of the EasyProtoBuf option schema that adds
  `optional bool future = 2` and keeps the current `easypb.cpp` extension
  number. Neither field has the `type` member, field number 1.
- `foreign_51000.pbs` contains `(validate.rules).string.min_len = 1`, using
  `tests/codegen/parser/differential/stubs/envoy/validate/validate.proto`.
  `validate.rules` occupies the old conflicting `FieldOptions` extension number
  51000. This fixture must remain encoded as 51000 when the EasyProtoBuf
  extension moves to another number.

Inspect a special fixture with `protoc --decode_raw` when changing it. After any
fixture update, run `codegen.custom_types` in both the full and descriptor-only
builds; the full build also verifies `.proto`/`.pbs` generated-output parity.
