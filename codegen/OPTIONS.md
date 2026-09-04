# EasyProtoBuf Codegen command-line options

Run `codegen --help` for the executable's generated option list.

## Input modes

EasyProtoBuf Codegen accepts either `.proto` source files or binary `FileDescriptorSet` files, conventionally named `.pbs`. All input files in one invocation are processed in the same mode.

Generation writes C++ header text to stdout. Redirect it to a file conventionally named `<schema>.pb.hpp`; generated output contains `#pragma once` and `inline` codec functions, so the header may be included from multiple translation units.

### Direct `.proto` input

The embedded parser can generate directly from a source schema:

```sh
codegen tutorial.proto >tutorial.pb.hpp
```

The parser recognizes and records imports but does not load them yet. Code generation stops when a source schema contains an unresolved imported type rather than guessing whether that type is a message or enum.

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

Descriptor-set input is useful when descriptor files are already available or when `protoc` is needed to resolve imported field types that the embedded parser cannot link. Definitions of imported C++ types are not emitted; they must be available to the generated header separately. See [Packages and C++ namespaces](GENERATED_CODE.md#packages-and-c-namespaces) for the generated names.

Each descriptor set must currently contain exactly one `FileDescriptorProto`. Avoid `protoc --include_imports` until target-file selection is implemented.

Without `--descriptor-set`, Codegen infers the common input mode from file extensions: an invocation containing both `.pbs` and non-`.pbs` names is rejected. `--descriptor-set` forces every positional input to be decoded as a binary descriptor set; it does not enable mixing descriptor sets with `.proto` source files.

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

Descriptor printing and benchmarking report unresolved imported types as warnings. Code generation from `.proto` source stops on such schemas instead of guessing whether an unresolved external type is a message or enum.

`--print-descriptor` and `--benchmark-parser` cannot be used together. `--benchmark-parser` accepts `.proto` source files, not descriptor-set input. Structural, packed-field, and C++ type generation options cannot be combined with either utility mode.

These parser utility modes are unavailable in a descriptor-set-only build.

## Help output

- `-h, --help` — print the normal usage/options help.
- `--groff` — produce groff-formatted help output.
- `--bash` — produce a Bash completion script.
