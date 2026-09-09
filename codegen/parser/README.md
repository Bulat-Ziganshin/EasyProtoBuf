# Embedded `.proto` parser

This directory contains the parser frontend used by [EasyProtoBuf Codegen](../README.md). It is intentionally split into independent components:

- [`proto_parser.cpp`](proto_parser.cpp) / [`proto_parser.hpp`](proto_parser.hpp) — lexer, recursive-descent parser, descriptor construction and source-position recording;
- [`pretty_printer.cpp`](pretty_printer.cpp) / [`pretty_printer.hpp`](pretty_printer.hpp) — optional descriptor-tree output;
- [`parser_benchmark.cpp`](parser_benchmark.cpp) / [`parser_benchmark.hpp`](parser_benchmark.hpp) — optional CLI benchmark helper;
- [`grammar/`](grammar/) — formal PEG descriptions and semantic notes.

The parser is built on the parser-independent `easypb_schema` core (`../schema.*`, `../schema_semantics.*`), which owns storage, diagnostics, and the shared semantic passes; the full source layout and module boundaries are documented in [Codegen internals](../INTERNALS.md). Only [`proto_parser.cpp`](proto_parser.cpp) belongs to the reusable parser library, together with its `easypb_schema` core dependency. The pretty-printer and benchmark are linked into the full `codegen` executable but are not dependencies of parser consumers.

The parser accepts a borrowed input buffer and returns a `ParsedProto` object that owns all strings retained by its `FileDescriptorProto` tree. The input buffer may be destroyed immediately after `parse_proto()` returns. `ParsedProto` is an alias for `easypb_schema::SchemaFile`; the shared descriptor structures are defined in [`../descriptor.pb.hpp`](../descriptor.pb.hpp).

Besides the descriptor tree, the parser fills the location-bearing import list and the authoritative `FileDescriptorProto` dependency lists (`dependency`, `public_dependency`, `weak_dependency`) in declaration order, and retains service names as minimal `ServiceDescriptorProto` entries for package-scope collision validation. Storage ownership, descriptor paths, and per-field source provenance come from the schema core (see [Codegen internals](../INTERNALS.md)).

Imports are parsed and recorded but are not loaded yet. By default the parser resolves type references against local declarations and reports imported types with the machine-readable diagnostic code `DIAGNOSTIC_UNRESOLVED_TYPE`; unresolved named types keep the `TYPE_MESSAGE` compatibility placeholder. Code generation refuses such schemas; descriptor printing and benchmarking continue with warnings. With `ParseOptions::defer_type_resolution`, the parser instead records declarations/imports/positions, applies only checks that need no type resolution, and leaves every named type unresolved (`has_type == false` with the raw spelling kept) without unresolved warnings, for the future loader to link. Kind-dependent checks for named types (packed legality, enum defaults) are deferred even when a local declaration could match: shadowing imports may change the resolved type after linking.

Note for parser-API consumers: rules that require knowing whether an unknown named type is an enum or a message are deferred rather than rejected. In particular, `packed=true` on an unresolvable type and a `default` on an unresolvable type now produce unresolved diagnostics (a warning in standalone mode, silence in deferred mode) instead of the former `packed option is valid only...` / `message fields cannot have defaults` errors. Complete linking still rejects genuinely invalid schemas.

General unsupported options are syntactically parsed and discarded. Options explicitly used by the trimmed Codegen descriptor are retained, including standard `default`/`packed` field options and EasyProtoBuf's `(easypb.cpp).type` field option. Type-template interpretation itself belongs to Codegen, not to the parser.

Top-level `service` and `rpc` declarations are syntactically validated and consumed, including unary and streaming request/response forms and service/method options. Only service names are retained, as minimal `ServiceDescriptorProto` entries, so they participate in package-scope collision validation; EasyProtoBuf Codegen generates message codecs rather than RPC client/server APIs.

Nested message declarations are retained recursively in `DescriptorProto::nested_type`; Codegen emits them as lexical C++ nested structs. Synthetic map-entry messages remain internal descriptor details and are not emitted as user-visible structs.

`FileDescriptorProto.syntax` follows [`protoc`](https://github.com/protocolbuffers/protobuf) representation: proto3 is stored explicitly, while proto2 is represented by an absent field 12.

Parser tests, the real-world differential corpus, and the comparison harness against `protoc` live under [`../../tests/codegen/parser/`](../../tests/codegen/parser/).
