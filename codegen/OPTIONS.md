# EasyProtoBuf Codegen command-line options

Run `codegen --help` for the executable's generated option list.

## Input modes

EasyProtoBuf Codegen accepts either `.proto` source files or binary `FileDescriptorSet` files, conventionally named `.pbs`. All input files in one invocation are processed in the same mode.

Generation writes one C++ header per selected target. With a single target and no `--out-dir`, the header text goes to stdout; redirect it to a file conventionally named `<schema>.pb.hpp`. With `--out-dir`, each target becomes `<logical name>.pb.hpp` below the output directory, subdirectories included. Generated output contains `#pragma once` and `inline` codec functions, so a header may be included from multiple translation units. Any command, input, link or generation error leaves stdout empty and reports a nonzero exit and a diagnostic on stderr, and a successful run checks that its stdout bytes reached the caller: a closed pipe, a full disk or a quota failure is that same error, not a silent success. Publication is per file, never a transaction across files (see [Target selection and output](#target-selection-and-output)). Running `codegen` with no arguments at all prints the help text and exits 0, as the reference compiler does; naming options but no input file is an error (`Missing input file.`, nonzero exit), so a build script whose file list expands to nothing cannot report success.

### Direct `.proto` input

The embedded parser can generate directly from a source schema:

```sh
codegen tutorial.proto >tutorial.pb.hpp
```

Search roots come from `-I/--proto_path`, ordered and repeatable; an empty root list means the current directory:

```sh
codegen -I schemas -I schemas/vendor api/report.proto >report.pb.hpp
```

An explicitly empty value is that same working directory, exactly as the reference compiler's `--proto_path=` is: `-I ""`, `--proto_path=` and `-I=` all add the working directory to the ordered roots instead of failing. Every other string option keeps the non-empty-value refusal, because there the value is a name.

The loader follows every import through those roots and the linker resolves each named type against the loaded set; a source schema whose import cannot be found fails with the loader's missing-import diagnostic rather than guessing whether the type is a message or enum. `-I` is valid only with `.proto` input. A positional input is a disk path when it names an existing file and a logical name otherwise; a spelling that names an existing file under no search root is retried as a logical name through the ordered roots before it is refused, exactly as the reference compiler does, so a stale copy in the working directory cannot turn the file the roots name into an error. An input spelling must not end with a path separator: `simple.proto/` names an entry the operating system does not resolve as that file, so it is refused (`input file name must not end with a path separator`) instead of being normalized back to the regular file. A separator on a search root or on `--out-dir` is ordinary directory syntax and stays accepted. Every positional input file is selected for generation, in the order given, and several inputs require `--out-dir`:

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

A single-file set without `--target` keeps the thin compatibility path: external type declarations only, and a reference into a missing dependency tolerated with a warning printed on stderr, as before plan 5. The tolerance covers the external **type**, not a default its declared kind decides: a message field may not carry a default, and an explicit enum default must be an identifier (its membership belongs to the dependency header). An implicit external enum default becomes `static_cast<EnumType>(0)` - the definition-independent value - so the member is never left indeterminate and the thin header equals the complete closure's; a cross-file enum whose first declared value is not zero is the one shape the complete link spells differently (`EnumType::FIRST_VALUE`). The generated header still includes every direct dependency it names, exactly as the complete link does. Naming an explicit `--target` links the loaded set strictly instead, so a missing dependency is an error. A descriptor file with no logical name has no `--target` spelling: an unnamed single-file set is reachable only through this no-target compatibility path. `protoc --include_imports` produces the complete closure intended for that mode; dependency headers it declares must exist for the generated C++ to compile. `--target` is valid only with `--descriptor-set`.

Descriptor-set input is useful when descriptor files are already available or when another tool produces them. Definitions of imported C++ types are never emitted inside the target's own header; the header includes the dependency header instead, in both link modes, so generate the dependencies as well. Fully qualified Protobuf names map to absolute C++ names in their package namespaces. See [Packages and C++ namespaces](GENERATED_CODE.md#packages-and-c-namespaces) for the generated names.

Without `--descriptor-set`, Codegen infers the common input mode from file extensions: an invocation containing both `.pbs` and non-`.pbs` names is rejected. The `.pbs` suffix is compared case-insensitively (`SET.PBS` and `set.Pbs` are descriptor sets), because a case-insensitive filesystem makes those spellings natural. `--descriptor-set` forces every positional input to be decoded as a binary descriptor set; it does not enable mixing descriptor sets with `.proto` source files.

## Option syntax

Options may appear before or after the positional inputs. A first `--` ends option parsing: every later token is an input spelling, even if it starts with `-`, and `--help` after it is a file name rather than the help switch (`protoc` has no such separator; this is a Codegen extension).

An unknown or misspelled option is an error, not a silent no-op: `--nope`, `-Z` and a typo such as `--no-has-field` are refused with `Unknown flag: <token>`. A bare `-` is not an option either: it is a positional input spelling and is refused by the loader like any other unknown input, instead of being dropped from the run. A required value is **the next token, whatever that token looks like**: `--out-dir --help` names an output directory `--help`, and `-I --out-dir` names a search root `--out-dir`. This is the POSIX `getopt` rule — the next argv element is the value, dash-prefixed or not — and a value is refused only when there is no next token at all (`Missing value for flag: <option>`). The inline form `--name=-value` writes the value directly and stays available. `--name=` (an explicitly empty value) is refused as well, with one exception: `-I/--proto_path` reads the empty value as the working directory (see [Direct `.proto` input](#direct-proto-input)). A short option writes its value as `-sVALUE` or `-s=VALUE`: in the short form one `=` immediately after the option letter is the separator, exactly as in the long form, so `-I=dir` searches `dir` and `-s=` is an explicitly empty value refused with the same sentence as the long form (`-s requires a non-empty value`). A value that really begins with `=` is written `-s==VALUE` or `-s =VALUE`. A switch takes no value, so `-c=` is an unknown flag. A `--` where an option is expected ends option parsing; in the value position it is the value, exactly as `getopt` takes it. (`protoc` refuses a next token that starts with `-`; this is a deliberate Codegen divergence, decided on 2026-10-22.)

## Target selection and output

- `--target NAME` — a logical `.proto` name to generate from a descriptor set. Repeatable; order preserved, duplicates dropped. Valid only with `--descriptor-set`. Without it, a single-file set uses the compatibility path and a multi-file set fails with target-selection guidance. A file with no logical name cannot be named by `--target`.
- `--out-dir DIR` — write one generated header per selected target below `DIR` instead of stdout. Relative subdirectories of a logical name are recreated below `DIR`. `DIR` may be given only once. Required whenever more than one header would be written.
- Without `--out-dir`, exactly one selected header is written to stdout; several selected headers without `--out-dir` fail with a guidance error.

Publication writes each generated header in place below `DIR`: the output directory and every parent directory of an output are created first, then each file is created or truncated and written. It refuses two targets that map to one output file, and it reports the first failure without claiming partial success. It is not a transaction across files: a run that fails after writing one header leaves the earlier headers written, and a process interrupted mid-write may leave that one header truncated — the same guarantee the reference compiler gives. Files are written in byte-wise output-name order, so on a case-folding filesystem two names that differ only in case (`api/x.pb.hpp`, `api/X.pb.hpp`) always leave the same survivor, whatever order the targets were named in — the rule the reference compiler's name-keyed output map applies.

Two bounds apply to the output side. An output name must be a logical path, so a name that escapes `DIR` by its own spelling (`../escape.pb.hpp`) is refused before anything is written; the reference compiler writes such a name one directory above the output root, and Codegen refuses it. And a path with more than 100 components is refused — a logical name that carries more than 100 of them is refused at the mapping layer, before the publisher creates anything. The same count is measured on the physical output path, so a pre-existing `--out-dir` deeper than 100 components is refused as well; that application of the logical-name bound to the caller's own root is the accepted E7/CR-36 limitation. When a regular file sits in the middle of `DIR`, the diagnostic names that component instead of claiming the requested `DIR` is a file. On Windows a network spelling that names a server but no share (`--out-dir //server`, `--out-dir //`) is refused as `cannot create directory (a network path names a server but no share)` before any directory exists; it is never read as the rooted `/server` under the current drive. A share spelling (`//server/share/...`) is a real network directory: nothing below the server is created, and a missing share fails where the header would be written.

## Structural options

- `-c, --no-class` — do not generate C++ structures or enum declarations. This is useful when adapting existing types: declare all message and enum types first, then include output containing only the external codec overloads.
- `-d, --no-decoder` — do not generate `decode(easypb::Decoder, T&)`.
- `-e, --no-encoder` — do not generate `encode(easypb::Encoder&, const T&)`.
- `-f, --no-has-fields` — do not generate `has_*` members. This also disables required-field checks.
- `--no-required` — do not check that proto2 required fields were present.
- `--no-default-values` - ignore explicit defaults specified in the schema. Singular enum fields still use the enum's first declared value as their implicit Protobuf default. If an imported enum's definition is unavailable, Codegen cannot determine that value and emits `static_cast<EnumType>(0)` instead.

## Packed repeated fields

- `-p, --packed` — encode every eligible repeated numeric field in packed form.
- `--no-packed` — encode every repeated field in unpacked form.

The two options cannot be used together. Without either override, Codegen
uses the field's `packed` option when it is written and the schema default
otherwise: a repeated primitive or enum field of a proto3 file is packed, and
every other field (including a proto2 field) is not. This is the same
effective value the descriptor merge compares (U-04), so two containers that
spell one logical file cannot change the wire format by argument order.

## Schema option validation

On the `.proto` path the parser validates standard options against a closed
table for the pinned reference compiler, `protoc` 30.1: the nine `*Options`
messages of `descriptor.proto` and their 47 known fields, excluding
`uninterpreted_option` and the nine editions `features` fields. Validation
covers the owner (file, message, field, oneof, enum, enum value, service,
method, extension range), the value kind, a second assignment of a singular
option, and the value-dependent rules protoc applies: `jstype` only on 64-bit
integers, `lazy`/`unverified_lazy` only on message fields (a named type is
checked after linking), `packed` only on repeated primitive or enum fields,
`map_entry` refused in source text, `allow_alias` only on enums,
`optimize_for` only on its enumerators, `message_set_wire_format` only on
messages, and `verification = UNVERIFIED` refused beside a non-empty extension
`declaration` list. An unknown unparenthesized name, an option in the wrong
owner, and a wrong value kind are refusals; the value itself is discarded.

This is validation, not feature support: no Java/C#/Go option changes the
generated C++. Only `default`, `json_name`, `packed` and `(easypb.cpp).type`
keep generated-code behavior. The descriptor door decodes the options whose
rules change a verdict - `packed`, `jstype`, `lazy`/`unverified_lazy`,
`message_set_wire_format`, `map_entry`, `allow_alias`, and the extension
range's `verification`/`declaration` - and asks the same rules over the
decoded values (`U-13`, `U-22`). Every other standard option is read as an
unknown field on the `.pbs` door, which is what protoc does with an option it
does not model here.

### The descriptor door's `jstype` and `lazy` rules

On `.pbs` input the values come from the decoded `FieldOptions`, not from a
source spelling:

- `jstype = JS_NORMAL` is the option's default and is accepted on every field
  kind; `JS_STRING` and `JS_NUMBER` are accepted only on `int64`, `uint64`,
  `sint64`, `fixed64` and `sfixed64` fields, and any other kind gets `jstype
  is only allowed on int64, uint64, sint64, fixed64 or sfixed64 fields`.
- `lazy = true` and `unverified_lazy = true` are accepted only on a message
  field; a scalar field gets `[lazy = true] can only be specified for
  submessage fields`. Either flag written as `false` is the default and is
  accepted on every field.
- An enumerator outside the enumerated `JSType` set is an unknown field, as in
  protoc's closed proto2 decoder, so it reads as JS_NORMAL and changes no
  verdict.

The source door answers the same two rules from the option's source position;
the descriptor door has no positions and reports the descriptor path.

### The descriptor door's extension range options

`ExtensionRangeOptions` is decoded too, and four combinations protoc refuses
are refused (`U-22`):

- an explicit `verification = UNVERIFIED` beside a non-empty `declaration`
  list (`cannot be marked UNVERIFIED while it has extension declarations`);
- a declaration number outside its own range;
- the same number declared twice in one range;
- a declaration that sets only one of `full_name`/`type` without being a
  `reserved` tombstone.

`descriptor.proto` marks `declaration` and `verification`
`[retention = RETENTION_SOURCE]`, so a descriptor written by protoc
`--descriptor_set_out` carries neither; the rules apply to a hand-written
`.pbs`, which is the input shape they exist for. The source door keeps an
aggregate option body opaque (exception `E13`), so these four are descriptor
rules.

A parenthesized custom option is deliberately opaque. Its defining extension
lives in an import this frontend does not link, so the name grammar and the
balanced value are checked while the name and value stay untyped; an unresolved
`(vendor.option)` is therefore accepted where protoc would refuse it. This is
the owner decision D2-A (2026-10-25), recorded as P1 exception E13 in
`build/verification/property-catalogue.md` and pinned by the
`codegen.options` lane. `(easypb.cpp).type` is the one custom name with
product semantics and keeps its existing handling; other `(easypb.cpp).*`
spellings are refused.

A dotted sub-field assignment on a singular message-typed option
(`[feature_support.edition_introduced = ...]`) is also outside the
validation-only table: it is refused as an unknown name, while protoc accepts
it. This stricter boundary is P1 exception E14 and is pinned by the same lane;
the aggregate spelling `[feature_support = { ... }]` is the supported form.

## C++ type options

`-s, --string-type arg (=::std::string)` selects the C++ type for all string and bytes fields. For decode-only messages, `std::string_view` or another non-owning view may be used when the decoded object never outlives the input buffer.

`-r, --repeated-type arg (=::std::vector)` selects the container for repeated fields. `{}` or `{0}` is replaced by the element type. If no placeholder is present, `<{}>` is appended. For example, `--repeated-type 'std::deque<{}>'` produces `std::deque<int32_t>`. Include the corresponding container header before the generated header.

`-m, --map-type arg (=::std::map)` selects the container for map fields. `{0}` and `{1}` are replaced by the key and value types. If no placeholders are present, `<{0},{1}>` is appended. Include the corresponding container header before the generated header.

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

`--print-descriptor` prints the parsed or decoded descriptor tree instead of generating C++. Messages, enums, services and their methods are printed; a method shows its endpoint types and, for a streaming method, its `client_streaming`/`server_streaming` markers. When an input cannot be read, the refusal appends the read layer's reason (`read access is denied`, `is a directory, not a schema file`, or the platform error) instead of a generic "cannot read file".

`--benchmark-parser` reads all input files before timing, runs one unmeasured warm-up round, and then parses complete corpus rounds for at least 100 ms by default. `--benchmark-ms N` changes the minimum measured parser time; `N` must be at least 100 and the option is valid only with `--benchmark-parser`. A read failure prints the file and the same read reason and exits 1, like every other CLI failure; the benchmark mode has no special exit code.

For a reproducible real-world workload, pass schemas from the [`tests/codegen/parser/differential/corpus/`](../tests/codegen/parser/differential/corpus/) directory. Tests on a 4 GHz Zen3 CPU showed 100–200 MB/s parsing throughput.

Descriptor printing and benchmarking report unresolved imported types as warnings. Code generation reports the warnings its load and link stored — a dropped `extend` declaration, a compatibility reference into a missing dependency — on stderr before it writes the header, with the exit code still 0; stdout stays the header only. The generation run stops only when an import cannot be found or a reference stays unresolved, instead of guessing whether the type is a message or enum.

`--print-descriptor` and `--benchmark-parser` cannot be used together. `--benchmark-parser` accepts `.proto` source files, not descriptor-set input. Structural, packed-field, and C++ type generation options cannot be combined with either utility mode. `-I/--proto_path` is a search root for `.proto` generation, not a code-generation option: the utility modes parse each file on its own and do not resolve imports, so `-I` with either mode is refused with its own sentence (`-I/--proto_path is a search-root option for .proto generation; descriptor print and parser benchmark modes do not resolve imports`) rather than being called a code-generation option.

These parser utility modes are unavailable in a descriptor-set-only build.

## Help output

- `-h, --help` — print the normal usage/options help.
- `--groff` — produce groff-formatted help output.
- `--bash` — produce a Bash completion script.
