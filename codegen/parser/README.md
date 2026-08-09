# Embedded `.proto` parser

This directory contains the parser frontend used by [EasyProtoBuf Codegen](../README.md). It is intentionally split into independent components:

- [`proto_parser.cpp`](proto_parser.cpp) / [`proto_parser.hpp`](proto_parser.hpp) — lexer, recursive-descent parser, descriptor construction and semantic checks;
- [`pretty_printer.cpp`](pretty_printer.cpp) / [`pretty_printer.hpp`](pretty_printer.hpp) — optional descriptor-tree output;
- [`parser_benchmark.cpp`](parser_benchmark.cpp) / [`parser_benchmark.hpp`](parser_benchmark.hpp) — optional CLI benchmark helper;
- [`grammar/`](grammar/) — formal PEG descriptions and semantic notes.

Only [`proto_parser.cpp`](proto_parser.cpp) belongs to the reusable parser library. The pretty-printer and benchmark are linked into the full `codegen` executable but are not dependencies of parser consumers.

The parser accepts a borrowed input buffer and returns a `ParsedProto` object that owns all strings retained by its `FileDescriptorProto` tree. The input buffer may be destroyed immediately after `parse_proto()` returns. The shared descriptor structures are defined in [`../descriptor.pb.hpp`](../descriptor.pb.hpp).

Imports are parsed and recorded but are not loaded yet. Unresolved imported types carry the machine-readable diagnostic code `DIAGNOSTIC_UNRESOLVED_TYPE`. Code generation refuses such schemas; descriptor printing and benchmarking continue with warnings.

Top-level `service` and `rpc` declarations are syntactically validated and consumed, including unary and streaming request/response forms and service/method options. They are intentionally not retained in the trimmed descriptor model because EasyProtoBuf Codegen generates message codecs rather than RPC client/server APIs.

`FileDescriptorProto.syntax` follows [`protoc`](https://github.com/protocolbuffers/protobuf) representation: proto3 is stored explicitly, while proto2 is represented by an absent field 12.

Parser tests, the real-world differential corpus, and the comparison harness against `protoc` live under [`../../tests/codegen/parser/`](../../tests/codegen/parser/).
