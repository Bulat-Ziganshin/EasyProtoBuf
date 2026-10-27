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

`main.cpp` selects the input/action mode and parses the command line (`-I/--proto_path` and `--target` are ordered and repeatable, `--out-dir` is single), loads all source roots or descriptor containers, selects targets, links the set, and generates through the core. One selected header goes to stdout, or every selected header is published through `header_output.cpp`. On Windows the command line reaches the program through `GetCommandLineW` with exactly one UTF-8 conversion before any loader runs.

## Source layout

- [`main.cpp`](main.cpp) — command-line parser, input-mode selection and validation, source/descriptor loading orchestration, target selection, link invocation, output publication, and utility-mode dispatch.
- [`codegen.cpp`](codegen.cpp) — the linked generator: message model and dependency analysis, the direct-dependency include planner (including the two compiler-support imports), imported enum-default indexing, and code generation for one target or a batch.
- [`cpp_names.hpp`](cpp_names.hpp) / [`cpp_names.cpp`](cpp_names.cpp) — validate package namespace names and convert absolute Protobuf identities to C++ spellings.
- [`descriptor.pb.hpp`](descriptor.pb.hpp) - internal trimmed C++ representation and EasyProtoBuf decoders for [`descriptor.proto`](https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto). Besides messages, enums, and fields it carries file dependency lists (`dependency`, `public_dependency`, `weak_dependency`) and service/method entries (a service name and, per method, its name, both endpoint types and the two streaming flags).
- [`schema.hpp`](schema.hpp) / [`schema.cpp`](schema.cpp) — parser-independent schema storage: string pool, diagnostics, `SchemaFile`/`SchemaSet` ownership, descriptor paths, source metadata, and the common import-edge binder (`bind_import_edges`). Builds without the source parser.
- [`schema_semantics.hpp`](schema_semantics.hpp) / [`schema_semantics.cpp`](schema_semantics.cpp) — shared symbol indexing (`SymbolIndex`, `index_file`) and local/strict field resolution (`resolve_file`) used by the parser, the import linker (`schema_linker.*`), and the compatibility pass. Builds without the source parser.
- [`schema_linker.hpp`](schema_linker.hpp) / [`schema_linker.cpp`](schema_linker.cpp) — import linking (`link_schema`): the deterministic name-sorted walk over edge binding, the one shared index, visibility, resolution and warning hygiene, the publish-on-success rule, and the non-owning `LinkedSchema` view. Carries both modes (`LINK_COMPLETE` and the thin-descriptor compatibility pass). Builds without the source parser.
- [`descriptor_adapter.hpp`](descriptor_adapter.hpp) / [`descriptor_adapter.cpp`](descriptor_adapter.cpp) — the common descriptor-ingestion adapter: owned deep copies of decoded `FileDescriptorProto`s into an owning `SchemaSet`, with every retained string interned into the file's own pool, logical-name validation and duplicate refusal. Builds without the source parser and never touches the disk.
- [`descriptor_loader.hpp`](descriptor_loader.hpp) / [`descriptor_loader.cpp`](descriptor_loader.cpp) — the standalone `.pbs` container reader outside `easypb_schema`: decodes a `FileDescriptorSet`, keeps a single unnamed file as the anonymous compatibility identity, retains the container path in `physical_name` for diagnostics, and forwards every named set through the adapter. The container path is an internal identity only: once the unnamed file's name is cleared, its index entry is removed, so no `--target` spelling can select a file the descriptor itself does not name.
- [`generation.hpp`](generation.hpp) — the public generation entry points `generate_header` (one target) and `generate_files` (explicit targets in request order) plus the owning `GeneratedFile{name, content}` result.
- [`generation_options.hpp`](generation_options.hpp) / [`generation_options.cpp`](generation_options.cpp) — `GenerationOptions` defaults and the shared validation/derived behavior every frontend applies.
- [`header_paths.hpp`](header_paths.hpp) / [`header_paths.cpp`](header_paths.cpp) — the pure logical-name → generated-header mapping (`.proto` → `.pb.hpp`, otherwise append `.pb.hpp`). No filesystem call and no case logic.
- [`header_output.hpp`](header_output.hpp) / [`header_output.cpp`](header_output.cpp) — the standalone output publisher, modelled on the reference compiler's `WriteAllToDisk`: duplicate output-name refusal, output-name validation, output-directory creation, per-output parent-directory creation, and an in-place write per file. Not part of the generation core. The containment, case-fold-collision, native-identity and atomic-replacement guards it used to carry were withdrawn by the plan-5 trim (`fbdcc0e`, 2026-10-11) as protoc-absent; the header records the measurements and where each name check now lives. One check was added back on 2026-10-21: the publisher validates every output name as a logical path itself, so a hand-built `GeneratedFile` cannot write outside the output root (the reference compiler writes such a name one directory above the root, and this project refuses it).
- [`logical_paths.hpp`](logical_paths.hpp) / [`logical_paths.cpp`](logical_paths.cpp) — pure logical import-name validation shared by every frontend. No filesystem calls or platform headers, so descriptor-set and future plugin input reuse the same spelling rules.
- [`file_paths.hpp`](file_paths.hpp) / [`file_paths.cpp`](file_paths.cpp) — native physical-path operations: joining, byte-wise root-prefix mapping, lexical absolute normalization, file status and reads for source input, plus the output-side directory creation and the plain in-place write. Owns every filesystem call and Windows path rule; never linked into the common schema core.
- [`proto_loader.hpp`](proto_loader.hpp) / [`proto_loader.cpp`](proto_loader.cpp) — source-only discovery, caching, deferred parsing, and root selection in `easypb_proto`. Built as `easypb_proto_loader` in full builds only; descriptor-only and future plugin targets never link it.
- [`easypb/options.proto`](easypb/options.proto) — EasyProtoBuf's Protobuf custom-option schema, currently including the per-field C++ type template.
- [`parser/`](parser/) — `.proto` lexer/parser, descriptor pretty-printer, and parser benchmark helper (see [parser/README.md](parser/README.md) for the frontend API and behavior).
- [`parser/README.md`](parser/README.md) — parser API, lifetime, dependency metadata, and unresolved-import/deferred behavior.
- [`parser/grammar/`](parser/grammar/) — formal grammar and semantic notes.
- [`utils.cpp`](utils.cpp) — common utility functions used by the generator.

The committed `descriptor.pb.hpp` intentionally uses the conventional `EASYPB_DESCRIPTOR_PB_HPP_INCLUDED` include guard for portability; normal Codegen output uses `#pragma once`.

## Parser boundary

The reusable parser library consists of [`parser/proto_parser.cpp`](parser/proto_parser.cpp) on top of the `easypb_schema` core (`schema.cpp`, `schema_semantics.cpp`, `logical_paths.cpp`). The pretty-printer and benchmark are optional helpers linked into the full `codegen` executable and are not dependencies of parser consumers. The schema core itself has no source-parser dependency: descriptor-only builds link it without any parser object files.

The parser returns an owned `easypb_schema::SchemaFile` (aliased as `ParsedProto`): the same trimmed descriptor structures that descriptor-set input decodes into, plus source provenance (`field_sources`, `declaration_locations`, location-bearing `imports`). Generation after the frontend boundary does not depend on whether the original input was `.proto` or `.pbs`.

Imports are parsed and recorded — in both the `imports` vector and the authoritative `FileDescriptorProto` dependency lists. Source input is then loaded through `load_source_files`, which follows every import through the search roots and binds the import edges; descriptor input is ingested through the adapter, which binds them too. `link_schema` resolves the whole loaded set, and generation reads only the linked view. A bare `parse_proto` remains available and leaves every named type unresolved without warnings so a caller can link a set itself. See [the parser documentation](parser/README.md) for the parser-level behavior and [Deferred and linked descriptor states](#deferred-and-linked-descriptor-states) for what the two states contain.

## Deferred and linked descriptor states

A `SchemaFile` produced by the source frontend in deferred mode (`ParseOptions::defer_type_resolution`, the mode `load_source_files` uses) is an **unlinked** descriptor: named types are still raw, and `link_schema` owns making them final. The two states are what every consumer has to know before it reads a schema:

| Descriptor state | Raw (deferred parse, before linking) | Linked (after a successful `link_schema`) |
| --- | --- | --- |
| scalar field | already final: the parser sets `type` and `has_type` | unchanged |
| named field (message, enum, or map value) | `type == 0`, `has_type == false`, and the source spelling kept in `type_name` (`has_type_name == true`), mirrored by `FieldSource::raw_type_name` for source input | `type_name` rewritten to the index's **absolute** spelling, with `type` and `has_type` set to the resolved kind; a proto2 group keeps its declared `TYPE_GROUP`, so a linked set no longer matches the bytes it was loaded from (one set, one link) |
| synthetic map-entry reference | unresolved like any other named type even though its entry is in the same file | resolved to the entry; the entry itself stays indexed and resolvable for the field the parser synthesized |
| kind-dependent checks (`packed` legality, enum defaults) | deferred, because a shadowing import may still change the resolved type | validated against the resolved kind |
| imports | recorded in the descriptor dependency lists **and** in the location-bearing `imports` list; after a bare `parse_proto` no target is bound yet (`edges` is empty), and [`load_source_files`](proto_loader.cpp) binds them before returning, so a loaded set already carries `edges` (the every-frontend rule: `bind_import_edges`, [`schema.hpp`](schema.hpp)) | bound by `bind_import_edges`, whose result drives visibility (`PublicExports`/`Visible(F)`) |
| warnings | a standalone parse may carry `DIAGNOSTIC_UNRESOLVED_TYPE` and deferred-default warnings | a successful complete link drops those stale warnings for the references it resolved and leaves every other code alone; the CLI prints every remaining warning on stderr before generation, with exit code 0 |

Two refusals in this layer are **deliberate divergences** from the reference
compiler, recorded here so a later round does not file them as defects (the
parity lane's `inverse` class, `tests/codegen/imports/data/parity/`):

- **Editions.** `edition = "2023"` is refused where libprotoc 30.1 accepts it,
  because this model carries no editions semantics and the classification
  refuses what it cannot model (A25). The two origins refuse it differently and
  both name editions: a descriptor set is refused by `link_schema` with
  `DIAGNOSTIC_UNSUPPORTED_SYNTAX`, and a `.proto` source statement is refused by
  the parser's `edition` branch with `editions syntax is not supported: only
  "proto2" and "proto3" are accepted` at the `edition` line (plan 5's parser
  branch, ledger D-44). Since S7e (`W-60`) the source refusal carries the same
  `DIAGNOSTIC_UNSUPPORTED_SYNTAX` the descriptor origin does, so the two
  origins agree on the code as well as on the verdict; the parity lane pins
  both: `d-unsupported-syntax-editions-inverse` at the descriptor origin and
  `s-unsupported-syntax-editions-inverse` at the source origin.
- **The map-entry shape sweep is reference-free.** A26's check runs on the
  descriptor alone, so an entry **no field references** is checked as well.
  protoc validates a map entry only through the referencing field's `is_map()`,
  so it ACCEPTS an unreferenced entry with an illegal (bytes) key that this layer
  refuses as `DIAGNOSTIC_MALFORMED_MAP_ENTRY` — the same deliberate strictness
  A26 already carried for an unreferenced entry missing its value member, now
  extended to the whole `ValidateMapEntry` shape (fix round r25, ledger `D-53`).

Descriptor-origin input is never raw: `.pbs` bytes carry `type` and `type_name` themselves, and that metadata is authoritative, so the linker compares it with what the name resolves to (`DIAGNOSTIC_KIND_MISMATCH`) instead of rewriting it.

Ingest normalizes the two file-presence bits that are not modeled information (`W-48`): a file's `name` is always present (the name is required and non-empty), and a non-empty `package` is always present, whatever bits a hand-built container carried. The wire decoder already sets them, so this is a no-op for `.pbs` bytes; `same_descriptor_file` compares the two strings, not their presence bits, so a normalized stored copy and an incoming copy of one file still merge.

Merge identity is the **effective semantic value**, not the presence bit (`U-04`, `U-07`, `U-52`). An absent field label is `LABEL_OPTIONAL`; an absent `packed` option is the value the emitter writes, computed by the one shared `effective_packed` helper (a proto3 repeated packable field is packed, proto2 and every unpackable field are not), so two containers cannot silently disagree about the wire format; an absent range endpoint is the protobuf default 0; a `cpp` extension group without its `type` member is no custom-type request, while a present-but-empty `type` stays a distinct (and refused) value so the merge cannot hide the refusal; and a default value is compared as the canonical text the adapter stores, so `0x0A` and `10` describe one int32 default. The unsupported syntax class also compares the edition **carrier**, so a class read from `edition` is not merged with one read from `syntax`.

`link_schema` runs the six stages of the end-state map — edge binding, one shared index, visibility, resolution, warning hygiene, publication — walking the files in an order that is a function of the logical file names alone (never of insertion order or pointer values), so the first reported failure is identical for every input order. Everything is built into a local `LinkedSchema` and assigned once at the end, so a failed link leaves the caller's result in its documented invalid empty state (`files == 0`) instead of a partial view. Linking *is* mutation of the caller's set: the resolved fields are overwritten in place.

Two bounds and two walks ride those stages (`U-05`, `U-06`). Between edge binding and the index, `link_schema` asks the **import-chain bound** (`max_import_chain_depth()`, 1 000 files): a descriptor-set graph arrives without the loader's filesystem walk, so the depth of the graph is measured here, with one iterative post-order pass, before any per-level cost is paid — and a chain deeper than the bound is refused with a sentence that names the depth, the limit and the file at the head of the chain (`import chain depth 20000 exceeds the limit of 1000 files`). The visibility stage's public-export walk is a post-order loop over an explicit heap stack for the same reason: one C++ call frame per `import public` link overflowed the process stack at ~1 950 links. Neither walk recurses, so the bound is a policy limit and never the stack's own.

The two modes share those stages and differ in their preconditions and in what the result reports:

- **`LINK_COMPLETE`** requires every import target to exist and every reference to resolve; on success `LinkedSchema::local_only` is `true`.
- **`LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY`** is the thin single-descriptor workflow: exactly one descriptor-origin file (a zero-file set and a two-file set fail the same precondition), missing dependency targets become null-target edges, and a reference the descriptor classifies itself is tolerated with one warning per external type name. `local_only` then describes the input — it stays `true` only when every dependency edge resolved and nothing external was tolerated. The mode itself is the caller's argument to `link_schema`, not a result field. The tolerance is a bounded compatibility guarantee, not a general unresolved-type escape hatch.
- The tolerance covers the external **type**, not a default its declared kind decides (`W-47`): a message or group field with a default is refused with `message fields cannot have defaults`, and an explicit enum default must be an identifier. Only membership of that identifier needs the definition, which the set does not carry, so it stays deferred to the dependency header. An *implicit* enum default cannot be deferred the same way: the emitter writes the definition-independent `static_cast<EnumType>(0)` (CR-08), which is also the spelling the complete link uses for a cross-file enum whose first value is zero, so the compatibility header equals the closure header (`W-68`, `P7`).
- A file whose `syntax` is neither absent, `"proto2"`, nor `"proto3"` is refused with `DIAGNOSTIC_UNSUPPORTED_SYNTAX` in stage 1 for **both** modes, before any resolution work, because the project cannot model editions semantics.

Ownership: `SchemaSet` owns the files, their descriptors and every retained string; `LinkedSchema` owns only its name keys and sets and borrows everything else, so the set must outlive the view and every generator call that reads it. The caller must not clear a member of a linked set while a view into it still exists.

## Source loader and logical names

Filesystem discovery lives in the source-only loader ([`proto_loader.cpp`](proto_loader.cpp) on top of `file_paths.cpp` and the deferred parser), while the common schema module owns the validated import graph (`bind_import_edges` plus [`logical_paths.cpp`](logical_paths.cpp)), so descriptor-set input and the future plugin reuse those checks without file I/O.

A logical name is the case-sensitive relative `/` path stored in `FileDescriptorProto.name` and `dependency` and reused for generated include names — a strict, platform-independent UTF-8 namespace, validated identically for every frontend with absolute spellings, backslashes, empty components, `.`/`..`, quotes, control characters, malformed UTF-8, and more than 100 components rejected. The first four of those rules are also what the reference compiler enforces when it opens a virtual path; the remaining checks are this project's deliberate strictness for foreign descriptor input. The component bound is the output side's reason as much as the input side's: the publisher creates one directory per component, so a name that carries thousands of them is a request for thousands of directories, and the reference compiler's own bound for a path-shaped thing (`kPackageLimit = 100`, `descriptor.cc:96`) is the magnitude used here.

The loader's observable behavior follows `protoc`: search roots are used in caller order with the first match winning, an argument naming an existing file is mapped to its logical name through a byte-wise (case-sensitive, never folded) root prefix with a shadowed input reported instead of renamed, an input outside all roots is an error, a denied access stops the search while absence alone advances it, every import including a weak one must exist, and a cycle is an error. Files are keyed by logical name only: each logical name is read once and parsed deferred with `file.name` holding that name and `physical_name` the disk path, while two names for one physical file simply load twice and two files under one name surface as duplicate definitions later — both exactly as in the reference compiler, which has no file-identity concept either. Import diagnostics name the failing file in `Diagnostic.file`, the declaration site in `location` when source coordinates exist, a machine-readable `DiagnosticCode` (`DIAGNOSTIC_INVALID/MISSING_IMPORT`, `DIAGNOSTIC_IMPORT_CYCLE`), and a discovery chain in quoted spelling for loader-level failures. Binder-level failures (a repeated dependency name, a cycle) report their own position and chain.

See [LOADER.md](LOADER.md) for the full explanation: the contract, a walkthrough of one `load_source_files` call, the invariants, the error classes, and the documented differences from the reference compiler.

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

The definition-dependency graph is fully built and validated before any generated text is emitted, so unsupported recursive schemas fail without leaving a partial result.

Generation settings live in `easypb_codegen::GenerationOptions` ([`generation_options.hpp`](generation_options.hpp)) with their defaults and the shared validation/derived behavior, so a core-only caller gets the same options the CLI does. Per-file generator state lives in an explicit context created per target, not in file-scope variables, so one process can generate several targets without leaking state; the user-facing behavior is documented in [OPTIONS.md](OPTIONS.md).

## Descriptor model

[`descriptor.pb.hpp`](descriptor.pb.hpp) is a trimmed descriptor header rather than the full official Protobuf runtime model. It contains the descriptor structures needed by the parser and generator and EasyProtoBuf decoders used for `.pbs` input.

Both frontends therefore produce data in the same model:

- `.proto` input: parser constructs the descriptor tree directly;
- `.pbs` input: [`descriptor_loader.cpp`](descriptor_loader.cpp) decodes the container; a single-file set may omit the logical name as the anonymous stdout compatibility identity, every named file is ingested through the adapter with the container path retained for diagnostics, and targets are selected explicitly.

Recognized EasyProtoBuf field options are preserved in this common descriptor model. For `.pbs` input, the descriptor decoder recognizes EasyProtoBuf's fixed extension number directly; Codegen does not implement a general Protobuf extension registry.

The `.pbs` decoder is strict about wire types. `descriptor.proto` declares one wire type per field, and a descriptor set is a schema definition, so a field written with another wire type is refused with a decode diagnostic instead of being reinterpreted: a `ReservedRange.start` spelled as `FIXED32`, for example, used to be converted into an integral value and silently change the decoded schema. The check lives in the descriptor decoder (`DescriptorDecoder` in [`descriptor.pb.hpp`](descriptor.pb.hpp), on [`easypb.hpp`](../include/easypb.hpp)'s `Reader::require_wire_type`), and applies to every descriptor message; a packed repeated numeric field still accepts both canonical spellings. The runtime decoder itself keeps its documented conversions for user messages (the `wire.type.conversions` contract), because a user message is read through the caller's own struct, not through a schema definition.

The options whose rules change a verdict are decoded rather than left as unknown fields: `FieldOptions.packed`, `jstype`, `lazy` and `unverified_lazy`, the extension range's `verification` and `declaration` list, `MessageOptions.message_set_wire_format` and `map_entry`, `EnumOptions.allow_alias`, and `(easypb.cpp).type`. An enumerator of a closed proto2 option enum that the pinned `descriptor.proto` does not declare is routed to the unknown fields, exactly as protoc's own decoder does, so it reads as the option's default and changes no verdict. The remaining options are validation-only on the source door and read as unknown fields on the descriptor door; [OPTIONS.md](OPTIONS.md) documents the user-facing rules.

`FieldDescriptorProto.proto3_optional` and the oneof list are model content, not decoration. The source parser sets the flag only for an explicit proto3 `optional` and then creates the synthetic one-field oneof itself: the name is derived from the field (`_f`, `X_f`, …), the declaration is appended after every oneof the user wrote, and the field's name is its declaration location (B12, `D1-A`), which is exactly what `protoc`'s `GenerateSyntheticOneofs` produces. The linker validates the invariant on both doors rather than re-creating it: the flag is legal only in a proto3 file and only on the sole member of its oneof; a *synthetic* oneof is recognized by shape (exactly one member field, and that field carries the flag - the same rule as `OneofDescriptor::is_synthetic`), so a hand-written `o` with that shape counts; every synthetic oneof must follow all others; and the members of one `oneof_index` must be a contiguous run in the field list, because `OneofDescriptor::field(i)` indexes them as an array. The emitter itself ignores oneof structure and writes each member as an ordinary presence field, so these rules protect the descriptor model and the two-door verdict, not the generated bytes.

## Default-value canonical text

Both doors store a numeric default as text, and both re-render it through the
same canonicalizer ([`schema_semantics.cpp`](schema_semantics.cpp)), so a
`.proto` spelling and the `.pbs` protoc writes from it cannot describe two
values: `010` is `8`, `0x10` is `16`, and a floating-point default is the
shortest text that survives the round trip. The floating rule mirrors protoc's
`io/strtod.cc`: a `double` tries 15 significant digits and widens to 17, a
`float` tries 6 and widens to 9. For the single-precision kind the round trip
goes through `strtof`, and a set `errno` counts as a mismatch because protoc's
`safe_strtof` asks the same question - this is why a finite subnormal (`1e-40`
on a `float` field) is stored and printed as `9.9999461e-41`, not the
six-digit `9.99995e-41`, and why the two doors and `--print-descriptor` agree
on it (`U-18`). `inf`, `-inf` and `nan` have fixed spellings; overflow is a
value, not a refusal (W-04). A leading `+` or leading whitespace is never
normalized away: those spellings are refusals, not values (W-03, W-40, `U-29`).

## Tests

Codegen tests live under [`../tests/codegen/`](../tests/codegen/), grouped by generated-code feature (`maps`, `enums`, `packages`, `nested`), parser/input behavior (`parser`), shared schema storage/semantics (`schema`), and package/import support (`imports` with `test_graph.cpp` for the common binder, `test_loader.cpp` and `data/loader/` for filesystem discovery, `test_linker.cpp` for descriptor-origin linking, `test_source_linker.cpp` for the source-driven path with the `data/linker/` fixtures and the `protoc` differential, `test_parity.cpp` with `data/parity/` for the two-way oracle lane, `test_descriptor_sets.cpp` for descriptor ingestion and lifetime, `test_in_memory.cpp` for the core-only in-memory path, `test_output.cpp` for the publisher, and `runtime/` plus `test_includes.cmake` for the two-TU runtime and include-planner lanes). See [BUILDING.md](BUILDING.md#testing) for the user-facing test commands and test-suite overview.
