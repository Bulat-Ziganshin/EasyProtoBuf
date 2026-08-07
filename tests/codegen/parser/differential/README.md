# Differential tests against protoc

This directory is test-only. Nothing here is linked into the production
`codegen` executable or the [`easypb_proto_parser`](../../../../codegen/parser/) library unless the optional
`EASYPB_BUILD_DIFFERENTIAL_TESTS` target is explicitly enabled.

The test performs the following steps for every file under [`corpus/`](corpus/):

1. run an external official `protoc` and request a binary `FileDescriptorSet`;
2. parse the same source with the EasyPB parser;
3. decode the official descriptor set with the trimmed [`../../../../codegen/descriptor.pb.cpp`](../../../../codegen/descriptor.pb.cpp);
4. recursively compare every descriptor field represented by that trimmed
   model: messages, nested types, enums, fields, map entries, oneofs, defaults,
   explicit packed presence/value, labels, numbers and type names.

Envoy and Kubernetes need imported definitions merely to let `protoc` resolve
and validate the source. Minimal files for those imports live under [`stubs/`](stubs/).
They are not loaded by the embedded EasyProtoBuf frontend and are not part of the production library.

Because the EasyPB parser deliberately does not load imports yet, the comparator permits only
two import-linker differences:

* a relative unresolved external type may match the suffix of protoc's absolute
  type name;
* an unresolved external enum may still be classified as `TYPE_MESSAGE`.

Local symbols and all other represented data must match exactly.

## Build and run

```sh
cmake -S . -B build-differential \
  -DEASYPB_BUILD_DIFFERENTIAL_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-differential
python3 tests/codegen/parser/differential/run.py \
  --protoc /path/to/protoc \
  --compare build-differential/parser_differential_compare
```

Run one file with `--case`, for example:

```sh
python3 tests/codegen/parser/differential/run.py \
  --protoc /path/to/protoc \
  --compare build-differential/parser_differential_compare \
  --case caffe.proto
```

## Verification baseline

The EasyPB parser was verified with [`libprotoc 3.13.0`](https://github.com/protocolbuffers/protobuf/releases/tag/v3.13.0). The compared fields are
stable `FileDescriptorProto` fields; newer protoc versions can be supplied to
the same runner to detect behavioral changes.
