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

Top-level `service` and `rpc` declarations are validated, and every method's name and endpoint types are linked, while message codecs from the same file are generated. A duplicate method name or an endpoint that is missing or is not a message type is refused; method options are validated and discarded. Codegen does not generate RPC client or server APIs.

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

A single-file set without `--target` keeps the thin compatibility path: external type declarations only, and a reference into a missing dependency tolerated with a warning printed on stderr, as before plan 5. The generated header still includes every direct dependency it names, exactly as the complete link does. Naming an explicit `--target` links the loaded set strictly instead, so a missing dependency is an error. A descriptor file with no logical name has no `--target` spelling: an unnamed single-file set is reachable only through this no-target compatibility path. `protoc --include_imports` produces the complete closure intended for that mode; dependency headers it declares must exist for the generated C++ to compile. `--target` is valid only with `--descriptor-set`.

Descriptor-set input is useful when descriptor files are already available or when another tool produces them. Definitions of imported C++ types are never emitted inside the target's own header; the header includes the dependency header instead, in both link modes, so generate the dependencies as well. Fully qualified Protobuf names map to absolute C++ names in their package namespaces. See [Packages and C++ namespaces](GENERATED_CODE.md#packages-and-c-namespaces) for the generated names.

Without `--descriptor-set`, Codegen infers the common input mode from file extensions: an invocation containing both `.pbs` and non-`.pbs` names is rejected. `--descriptor-set` forces every positional input to be decoded as a binary descriptor set; it does not enable mixing descriptor sets with `.proto` source files.

## Option syntax

Options may appear before or after the positional inputs. A first `--` ends option parsing: every later token is an input spelling, even if it starts with `-`, and `--help` after it is a file name rather than the help switch (`protoc` has no such separator; this is a Codegen extension).

An unknown or misspelled option is an error, not a silent no-op: `--nope`, `-Z` and a typo such as `--no-has-field` are refused with `Unknown flag: <token>`. A required value is **the next token, whatever that token looks like**: `--out-dir --help` names an output directory `--help`, and `-I --out-dir` names a search root `--out-dir`. This is the POSIX `getopt` rule — the next argv element is the value, dash-prefixed or not — and a value is refused only when there is no next token at all (`Missing value for flag: <option>`). The inline form `--name=-value` writes the value directly and stays available. `--name=` (an explicitly empty value) is refused as well. A `--` where an option is expected ends option parsing; in the value position it is the value, exactly as `getopt` takes it. (`protoc` refuses a next token that starts with `-`; this is a deliberate Codegen divergence, decided on 2026-10-22.)

## Target selection and output

- `--target NAME` — a logical `.proto` name to generate from a descriptor set. Repeatable; order preserved, duplicates dropped. Valid only with `--descriptor-set`. Without it, a single-file set uses the compatibility path and a multi-file set fails with target-selection guidance. A file with no logical name cannot be named by `--target`.
- `--out-dir DIR` — write one generated header per selected target below `DIR` instead of stdout. Relative subdirectories of a logical name are recreated below `DIR`. `DIR` may be given only once. Required whenever more than one header would be written.
- Without `--out-dir`, exactly one selected header is written to stdout; several selected headers without `--out-dir` fail with a guidance error.

Publication writes each generated header in place below `DIR`: the output directory and every parent directory of an output are created first, then each file is created or truncated and written. It refuses two targets that map to one output file, and it reports the first failure without claiming partial success. It is not a transaction across files: a run that fails after writing one header leaves the earlier headers written, and a process interrupted mid-write may leave that one header truncated — the same guarantee the reference compiler gives. Files are written in byte-wise output-name order, so on a case-folding filesystem two names that differ only in case (`api/x.pb.hpp`, `api/X.pb.hpp`) always leave the same survivor, whatever order the targets were named in — the rule the reference compiler's name-keyed output map applies.

Two bounds apply to the output side. An output name must be a logical path, so a name that escapes `DIR` by its own spelling (`../escape.pb.hpp`) is refused before anything is written; the reference compiler writes such a name one directory above the output root, and Codegen refuses it. And a path with more than 100 components is refused — a logical name that carries more than 100 of them is refused at the mapping layer, before the publisher creates anything.

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

Descriptor printing and benchmarking report unresolved imported types as warnings. Code generation reports the warnings its load and link stored — a dropped `extend` declaration, a compatibility reference into a missing dependency — on stderr before it writes the header, with the exit code still 0; stdout stays the header only. The generation run stops only when an import cannot be found or a reference stays unresolved, instead of guessing whether the type is a message or enum.

`--print-descriptor` and `--benchmark-parser` cannot be used together. `--benchmark-parser` accepts `.proto` source files, not descriptor-set input. Structural, packed-field, and C++ type generation options cannot be combined with either utility mode.

These parser utility modes are unavailable in a descriptor-set-only build.

## Help output

- `-h, --help` — print the normal usage/options help.
- `--groff` — produce groff-formatted help output.
- `--bash` — produce a Bash completion script.
