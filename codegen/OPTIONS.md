# EasyProtoBuf Codegen command-line options

Run `codegen --help` for the executable's generated option list.

## Input modes

EasyProtoBuf Codegen accepts either `.proto` source files or binary `FileDescriptorSet` files, conventionally named `.pbs`. All input files in one invocation are processed in the same mode.

Generation writes one C++ header per selected target. With a single target and no `--out-dir`, the header text goes to stdout; redirect it to a file conventionally named `<schema>.pb.hpp`. With `--out-dir`, each target becomes `<logical name>.pb.hpp` below the output directory, subdirectories included. Generated output contains `#pragma once` and `inline` codec functions, so a header may be included from multiple translation units. Any command, input, link or generation error leaves stdout empty and reports a nonzero exit and a diagnostic on stderr; publication is per file, never a transaction across files (see [Target selection and output](#target-selection-and-output)).

### Direct `.proto` input

The embedded parser can generate directly from a source schema:

```sh
codegen tutorial.proto >tutorial.pb.hpp
```

Search roots come from `-I/--proto_path`, ordered and repeatable; an empty root list means the current directory:

```sh
codegen -I schemas -I schemas/vendor api/report.proto >report.pb.hpp
```

The loader follows every import through those roots and the linker resolves each named type against the loaded set; a source schema whose import cannot be found fails with the loader's missing-import diagnostic rather than guessing whether the type is a message or enum. `-I` is valid only with `.proto` input. Every positional input file is selected for generation, in the order given, and several inputs require `--out-dir`:

```sh
codegen -I schemas --out-dir build/generated common/state.proto common/node.proto api/report.proto
```

`--target` is not used with source input: the input list itself is the selection.

Top-level `service` and `rpc` declarations are validated and skipped while message codecs from the same file are generated. Codegen does not generate RPC client or server APIs.

### Descriptor-set input

Use the official [`protoc`](https://github.com/protocolbuffers/protobuf/releases) compiler to create a descriptor set:

```sh
protoc tutorial.proto -otutorial.pbs
```

Then either pass a file with a `.pbs` extension or explicitly select descriptor-set mode:

```sh
codegen tutorial.pbs >tutorial.pb.hpp
codegen --descriptor-set tutorial.pbs >tutorial.pb.hpp
```

A descriptor set may carry any number of files. With several files, name the files to generate with `--target` (repeatable; the requested order is preserved and duplicates are dropped):

```sh
protoc -I schemas --include_imports \
    --descriptor_set_out=project.pbs \
    common/state.proto common/node.proto api/report.proto
codegen --descriptor-set project.pbs \
    --target common/state.proto --target common/node.proto --target api/report.proto \
    --out-dir build/generated
```

A single-file set without `--target` keeps the thin compatibility path: external type declarations only, automatic dependency includes omitted, and a reference into a missing dependency tolerated with a warning, as before plan 5. Naming an explicit `--target` links the loaded set strictly instead, so a missing dependency is an error. `protoc --include_imports` produces the complete closure intended for that mode; dependency headers it declares must exist for the generated C++ to compile. `--target` is valid only with `--descriptor-set`.

Descriptor-set input is useful when descriptor files are already available or when another tool produces them. Definitions of imported C++ types are never emitted inside the target's own header; a complete link includes the dependency header instead, so generate the dependencies as well. Fully qualified Protobuf names map to absolute C++ names in their package namespaces. See [Packages and C++ namespaces](GENERATED_CODE.md#packages-and-c-namespaces) for the generated names.

Without `--descriptor-set`, Codegen infers the common input mode from file extensions: an invocation containing both `.pbs` and non-`.pbs` names is rejected. `--descriptor-set` forces every positional input to be decoded as a binary descriptor set; it does not enable mixing descriptor sets with `.proto` source files.

## Target selection and output

- `--target NAME` — a logical `.proto` name to generate from a descriptor set. Repeatable; order preserved, duplicates dropped. Valid only with `--descriptor-set`. Without it, a single-file set uses the compatibility path and a multi-file set fails with target-selection guidance.
- `--out-dir DIR` — write one generated header per selected target below `DIR` instead of stdout. Relative subdirectories of a logical name are recreated below `DIR`. `DIR` may be given only once. Required whenever more than one header would be written.
- Without `--out-dir`, exactly one selected header is written to stdout; several selected headers without `--out-dir` fail with a guidance error.

Publication replaces each existing target through a temporary file in the same directory, so an interrupted replacement never truncates a target. Per-file replacement is the guarantee; it is not a transaction across files, so a run that fails after replacing one header leaves earlier headers in their new state and the failing target unchanged.

## Structural options

- `-c, --no-class` — do not generate C++ structures or enum declarations. This is useful when adapting existing types: declare all message and enum types first, then include output containing only the external codec overloads.
- `-d, --no-decoder` — do not generate `decode(easypb::Decoder, T&)`.
- `-e, --no-encoder` — do not generate `encode(easypb::Encoder&, const T&)`.
- `-f, --no-has-fields` — do not generate `has_*` members. This also disables required-field checks.
- `--no-required` — do not check that proto2 required fields were present.
- `--no-default-values` — ignore explicit defaults specified in the schema. Singular enum fields still use the enum's first declared value as their implicit Protobuf default. If an imported enum's definition is unavailable, Codegen cannot determine that value and emits no initializer.

## Packed repeated fields

- `-p, --packed` — encode every eligible repeated numeric field in packed form.
- `--no-packed` — encode every repeated field in unpacked form.

The two options cannot be used together. Without either override, Codegen follows the field's schema option where present and the normal proto2/proto3 default behavior otherwise.

## C++ type options

`-s, --string-type arg (=std::string)` selects the C++ type for all string and bytes fields. For decode-only messages, `std::string_view` or another non-owning view may be used when the decoded object never outlives the input buffer.

`-r, --repeated-type arg (=std::vector)` selects the container for repeated fields. `{}` or `{0}` is replaced by the element type. If no placeholder is present, `<{}>` is appended. For example, `--repeated-type 'std::deque<{}>'` produces `std::deque<int32_t>`. Include the corresponding container header before the generated header.

`-m, --map-type arg (=std::map)` selects the container for map fields. `{0}` and `{1}` are replaced by the key and value types. If no placeholders are present, `<{0},{1}>` is appended. Include the corresponding container header before the generated header.

C++ types can also be specified per field with the `(easypb.cpp).type` custom option. See [C++ field type generation](GENERATED_CODE.md#c-field-type-generation) for details.

`--allow-self-recursive-containers` — allow a message type to refer to itself in repeated fields and map values. See [Self-recursive containers](GENERATED_CODE.md#self-recursive-containers) for details.

See [Generated C++ code](GENERATED_CODE.md) for the generated enum/map semantics and optional code insertion macros.

## Parser utility modes

When parser support is included, Codegen can also print the descriptor tree or benchmark `.proto` parsing:

```sh
codegen --print-descriptor tutorial.proto
codegen --descriptor-set --print-descriptor tutorial.pbs
codegen --benchmark-parser [--benchmark-ms 500] a.proto b.proto
```

`--print-descriptor` prints the parsed or decoded descriptor tree instead of generating C++.

`--benchmark-parser` reads all input files before timing, runs one unmeasured warm-up round, and then parses complete corpus rounds for at least 100 ms by default. `--benchmark-ms N` changes the minimum measured parser time; `N` must be at least 100 and the option is valid only with `--benchmark-parser`.

For a reproducible real-world workload, pass schemas from the [`tests/codegen/parser/differential/corpus/`](../tests/codegen/parser/differential/corpus/) directory. Tests on a 4 GHz Zen3 CPU showed 100–200 MB/s parsing throughput.

Descriptor printing and benchmarking report unresolved imported types as warnings. Code generation links the loaded set first, so imported types resolve; the run stops only when an import cannot be found or a reference stays unresolved, instead of guessing whether the type is a message or enum.

`--print-descriptor` and `--benchmark-parser` cannot be used together. `--benchmark-parser` accepts `.proto` source files, not descriptor-set input. Structural, packed-field, and C++ type generation options cannot be combined with either utility mode.

These parser utility modes are unavailable in a descriptor-set-only build.

## Help output

- `-h, --help` — print the normal usage/options help.
- `--groff` — produce groff-formatted help output.
- `--bash` — produce a Bash completion script.
