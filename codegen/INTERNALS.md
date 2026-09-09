# EasyProtoBuf Codegen internals

This document describes the implementation of Codegen itself. It is intended mainly for contributors; users of the generated API should start with [README.md](README.md), [OPTIONS.md](OPTIONS.md), and [GENERATED_CODE.md](GENERATED_CODE.md).

## Pipeline

Both input paths converge on the same `FileDescriptorProto` representation before C++ generation:

```text
.proto source
    |
    v
parser/proto_parser.cpp
    |
    v
FileDescriptorProto
    |
    +--------------------+
                         |
.pbs FileDescriptorSet   |
    |                    |
    v                    |
descriptor.pb.hpp decoder|
    |                    |
    +--------------------+
             |
             v
        codegen.cpp
             |
             v
      generated C++ header
```

`main.cpp` selects the input/action mode, reads files, invokes the parser or descriptor decoder, reports parser diagnostics, and writes the generated header to stdout.

## Source layout

- [`main.cpp`](main.cpp) — command-line parser, input-mode selection, file I/O, descriptor decoding, parser diagnostics, and utility-mode dispatch.
- [`codegen.cpp`](codegen.cpp) — translates `FileDescriptorProto` into C++ code.
- [`cpp_names.hpp`](cpp_names.hpp) / [`cpp_names.cpp`](cpp_names.cpp) — validate package namespace names and convert absolute Protobuf identities to C++ spellings.
- [`descriptor.pb.hpp`](descriptor.pb.hpp) — internal trimmed C++ representation and EasyProtoBuf decoders for [`descriptor.proto`](https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto). Besides messages, enums, and fields it carries file dependency lists (`dependency`, `public_dependency`, `weak_dependency`) and minimal service names.
- [`schema.hpp`](schema.hpp) / [`schema.cpp`](schema.cpp) — parser-independent schema storage: string pool, diagnostics, `SchemaFile`/`SchemaSet` ownership, descriptor paths, and source metadata. Builds without the source parser.
- [`schema_semantics.hpp`](schema_semantics.hpp) / [`schema_semantics.cpp`](schema_semantics.cpp) — shared symbol indexing (`SymbolIndex`, `index_file`) and local/strict field resolution (`resolve_file`) used by the parser and the future linker. Builds without the source parser.
- [`easypb/options.proto`](easypb/options.proto) — EasyProtoBuf's Protobuf custom-option schema, currently including the per-field C++ type template.
- [`parser/`](parser/) — `.proto` lexer/parser, descriptor pretty-printer, and parser benchmark helper (see [parser/README.md](parser/README.md) for the frontend API and behavior).
- [`parser/README.md`](parser/README.md) — parser API, lifetime, dependency metadata, and unresolved-import/deferred behavior.
- [`parser/grammar/`](parser/grammar/) — formal grammar and semantic notes.
- [`utils.cpp`](utils.cpp) — common utility functions used by the generator.

The committed `descriptor.pb.hpp` intentionally uses the conventional `EASYPB_DESCRIPTOR_PB_HPP_INCLUDED` include guard for portability; normal Codegen output uses `#pragma once`.

## Parser boundary

The reusable parser library consists of [`parser/proto_parser.cpp`](parser/proto_parser.cpp) on top of the `easypb_schema` core (`schema.cpp`, `schema_semantics.cpp`). The pretty-printer and benchmark are optional helpers linked into the full `codegen` executable and are not dependencies of parser consumers. The schema core itself has no source-parser dependency: descriptor-only builds link it without any parser object files.

The parser returns an owned `easypb_schema::SchemaFile` (aliased as `ParsedProto`): the same trimmed descriptor structures that descriptor-set input decodes into, plus source provenance (`field_sources`, `declaration_locations`, location-bearing `imports`). Generation after the frontend boundary does not depend on whether the original input was `.proto` or `.pbs`.

Imports are parsed and recorded — in both the `imports` vector and the authoritative `FileDescriptorProto` dependency lists — but are not loaded yet. Unresolved imported types are therefore a frontend diagnostic; `main.cpp` refuses to pass such source schemas to the generator. The deferred `parse_proto` overload additionally leaves every named type unresolved without warnings so the future loader can link a set of files. See [the parser documentation](parser/README.md) for the parser-level behavior.

## Protobuf and C++ names

The generator keeps Protobuf identity separate from C++ spelling. Message metadata stores the absolute Protobuf name (for example `.foo.bar.Outer.Inner`), the package-local C++ spelling (`Outer::Inner`), and the absolute C++ spelling (`::foo::bar::Outer::Inner`). Field and enum-default references use the absolute C++ spelling.

[`cpp_names.cpp`](cpp_names.cpp) owns package validation and the shared name conversions. The package itself is emitted as C++11-compatible nested namespace blocks around top-level enums, message structures, and free codec overloads. Runtime names such as `::easypb::Encoder` and built-in standard-library/default integer spellings are absolute so an application namespace cannot shadow them. User-supplied C++ type fragments remain literal.

Insertion macros deliberately keep their pre-existing `EASYPB_{TYPE}_...` naming convention and do not include package components. This avoids changing the insertion-hook mechanism as part of namespace generation.

## Message model and dependency analysis

`codegen.cpp` first builds a lexical `MessageInfo` tree for real messages. Synthetic map-entry messages remain in `DescriptorProto::nested_type` as map metadata and are deliberately excluded from the lexical C++ message tree.

The generator then indexes messages by absolute Protobuf name and resolves field type references to message owners. `MessageInfo::dependencies` is specifically the **C++ definition-dependency graph**: it records references whose target type must already be complete before the owning message definition is emitted. It is therefore not necessarily the complete Protobuf type-reference graph. Dependencies are normally recorded for message fields and for nested enums whose owning message must already be declared.

The decision is centralized in `requires_definition_dependency()`. Normally a local message-valued field requires a definition dependency. When `--allow-self-recursive-containers` is enabled, a direct self-reference through a repeated field or map value does not add such an edge because the configured C++ container is assumed to support an incomplete contained type. For the default repeated representation this is already an existing Codegen portability requirement: `DescriptorProto` itself contains `std::vector<DescriptorProto> nested_type`, so the project's compiler matrix continuously exercises self-recursive `std::vector<T>`, including C++11 configurations. Recursive maps and user-selected containers remain separate library/container capabilities. This semantic decision point is intentionally suitable for future indirect/pointer-like message storage, which may likewise not require a complete target type at structure-definition time; no pointer behavior is implemented today.

Before emitting text, Codegen validates the resulting definition-dependency graph. It still rejects singular self-recursion, mutual recursion, ancestor recursion, and other recursive value dependencies that cannot be represented by the generated by-value C++ layout.

For valid acyclic schemas, sibling lexical subtrees are put into a stable topological order. This handles forward references while preserving source order where dependencies do not require reordering. The same dependency rule is used for top-level messages and nested siblings.

## Generation

After dependency validation, generation is a recursive pass over the ordered message tree:

1. Emit top-level enum declarations.
2. Emit each message structure, including its nested enums and nested messages.
3. Emit field definitions and optional `has_*` state.
4. Build the corresponding encoder and decoder bodies.
5. Emit free `inline encode(...)` / `decode(...)` overloads for every real message, including nested messages.

The definition-dependency graph is fully built and validated before generated text reaches `main.cpp`, so unsupported recursive schemas fail without leaving a partial generated file.

Generator-wide command-line settings are stored in the `option` structure in [`codegen.cpp`](codegen.cpp). The user-facing behavior of those settings is documented in [OPTIONS.md](OPTIONS.md).

## Descriptor model

[`descriptor.pb.hpp`](descriptor.pb.hpp) is a trimmed descriptor header rather than the full official Protobuf runtime model. It contains the descriptor structures needed by the parser and generator and EasyProtoBuf decoders used for `.pbs` input.

Both frontends therefore produce data in the same model:

- `.proto` input: parser constructs the descriptor tree directly;
- `.pbs` input: EasyProtoBuf decodes the `FileDescriptorSet`, after which `main.cpp` requires exactly one `FileDescriptorProto`.

Recognized EasyProtoBuf field options are preserved in this common descriptor model. For `.pbs` input, the descriptor decoder recognizes EasyProtoBuf's fixed extension number directly; Codegen does not implement a general Protobuf extension registry.

## Tests

Codegen tests live under [`../tests/codegen/`](../tests/codegen/), grouped by generated-code feature (`maps`, `enums`, `packages`, `nested`) and parser/input behavior (`parser`). See [BUILDING.md](BUILDING.md#testing) for the user-facing test commands and test-suite overview.
