# EasyProtoBuf Codegen

EasyProtoBuf Codegen accepts either a `.proto` source file or a binary descriptor set (`.pbs`).

For direct source input:

```sh
codegen tutorial.proto >tutorial.pb.hpp
```

For descriptor-set input, first use the official [`protoc`](https://github.com/protocolbuffers/protobuf/releases) compiler to create the descriptor set:

```sh
protoc tutorial.proto -otutorial.pbs
```

Then run EasyProtoBuf Codegen using either form:

```sh
codegen tutorial.pbs >tutorial.pb.hpp
codegen --descriptor-set tutorial.pbs >tutorial.pb.hpp
```

Codegen writes a header to stdout. The conventional output name is `<schema>.pb.hpp`, and generated output contains `#pragma once`, so it may be included from multiple translation units.

The committed `descriptor.pb.hpp` is an internal trimmed descriptor header and intentionally keeps the legacy `EASYPB_DESCRIPTOR_PB_CPP_INCLUDED` include guard for portability; normal Codegen output still uses `#pragma once`.

Descriptor-set input can be useful, for example, when descriptor files are already available, when that workflow is preferred, or when a schema uses imports that the built-in parser cannot link yet. A descriptor set must currently contain exactly one `FileDescriptorProto`; avoid `protoc --include_imports` until target-file selection is implemented.

The generated C++ header contains plain structures followed by free codec overloads:

```cpp
struct Message
{
    int32_t id = 0;
};

inline void encode(easypb::Encoder &pb, const Message &x)
{
    pb.put_int32(1, x.id);
}

inline void decode(easypb::Decoder pb, Message &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_int32(&x.id); break;
            default: pb.skip_field();
        }
    }
}
```

The codec overloads are found through ADL (argument-dependent lookup), so they must be defined
either in the same namespace as the message type or in `easypb`. See [Using the API](../README.md#using-the-api)
for details.

Files:
- [main.cpp](main.cpp) — command-line parser and file I/O
- [codegen.cpp](codegen.cpp) — translates `FileDescriptorProto` into C++ code
- [descriptor.pb.hpp](descriptor.pb.hpp) — C++ header with structures and EasyProtoBuf decoders for
  [`descriptor.proto`](https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto)
- [parser/](parser/) — `.proto` lexer/parser, descriptor pretty-printer and parser benchmark helper
- [parser/README.md](parser/README.md) — parser API, lifetime and unresolved-import behavior
- [parser/grammar/](parser/grammar/) — formal grammar and semantic notes
- [utils.cpp](utils.cpp) — common utility functions

## Parser utility modes

When parser support is included, Codegen can also print the descriptor tree or benchmark `.proto` parsing:

```sh
codegen --print-descriptor tutorial.proto
codegen --descriptor-set --print-descriptor tutorial.pbs
codegen --benchmark-parser a.proto b.proto
codegen --benchmark-parser --benchmark-ms 500 a.proto b.proto
```

`--benchmark-parser` reads all input files before timing, runs one unmeasured warm-up round, and then parses complete corpus rounds for at least 100 ms by default. Tests on a varied real-world corpus showed roughly **100–200 MB/s** parsing throughput, depending on schema structure and compiler.

The parser recognizes and records imports but does not load them yet. Descriptor printing and benchmarking report unresolved imported types as warnings. Code generation from `.proto` source stops rather than guessing whether an unresolved external type is a message or enum; descriptor-set input can be used for such schemas.

The parser implementation and its documentation live in [`parser/`](parser/). Parser tests are kept separately under [`../tests/codegen/parser/`](../tests/codegen/parser/).

## Optional descriptor-set-only build

If only descriptor-set input is needed, parser support can be omitted at build time:

```sh
cmake -S . -B build-lite -DEASYPB_CODEGEN_WITH_PROTO_PARSER=OFF
cmake --build build-lite
```

With xmake:

```sh
xmake f --codegen_parser=n
xmake
```

In this configuration nothing from [`parser/`](parser/) is compiled. Codegen accepts descriptor sets through either `--descriptor-set file.pbs` or the implicit `.pbs` form.

## Quick verification

From the repository root, the normal CMake test suite exercises both input formats and the parser utilities:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The [`codegen.modes`](../tests/codegen/parser/test_codegen_modes.cmake) test checks explicit and implicit descriptor-set input, `.proto` versus `.pbs` generated-code equivalence, proto2/proto3 packed behavior, descriptor printing, parser benchmarking, unresolved-type handling, and invalid empty/multi-file descriptor sets.

The [`codegen.maps`](../tests/codegen/maps/test_codegen_maps.cmake) test covers scalar, enum and message-valued map generation, custom map containers, malformed map descriptors, and `.proto`/`.pbs` equivalence. `codegen.maps.runtime` compiles the generated ADL codecs and verifies scalar, enum and message-valued map round trips in both full and descriptor-set-only builds.

The parser unit tests are in [`../tests/codegen/parser/test_parser.cpp`](../tests/codegen/parser/test_parser.cpp).

To verify the descriptor-set-only build separately:

```sh
cmake -S . -B build-lite -DEASYPB_CODEGEN_WITH_PROTO_PARSER=OFF
cmake --build build-lite
ctest --test-dir build-lite --output-on-failure
```

## Structural options

- `-c, --no-class` — do not generate C++ structures. This is useful when adapting existing types:
  declare the types first, then include output containing only the external codec overloads.
- `-d, --no-decoder` — do not generate `decode(easypb::Decoder, T&)`.
- `-e, --no-encoder` — do not generate `encode(easypb::Encoder&, const T&)`.
- `-f, --no-has-fields` — do not generate `has_*` members. This also disables required-field checks.
- `--no-required` — do not check that proto2 required fields were present.
- `--no-default-values` — ignore defaults specified in the schema.
- `-p, --packed` — encode every eligible repeated numeric field in packed form.
- `--no-packed` — encode every repeated field in unpacked form.

## C++ type options

`-s, --string-type arg (=std::string)` selects the C++ type for all string and bytes fields.
For decode-only messages, `std::string_view` or another non-owning view may be used when the decoded object
never outlives the input buffer.

`-r, --repeated-type arg (=std::vector)` selects the container for repeated fields.
`{}` or `{0}` is replaced by the element type. If no placeholder is present, `<{}>` is appended.
For example, `--repeated-type 'std::deque<{}>'` produces `std::deque<int32_t>`.
Include the corresponding container header before the generated header.

`-m, --map-type arg (=std::map)` selects the container for map fields.
`{0}` and `{1}` are replaced by the key and value types. If no placeholders are present,
`<{0},{1}>` is appended. Include the corresponding container header before the generated header.

Codegen supports scalar, enum and message map values. Enum values are represented as `int32_t`.
Message-valued maps may use message types that Codegen can currently emit; nested message definitions are not generated yet.

## Code insertion points

For each generated message type `{TYPE}`, Codegen recognizes four optional insertion macros:

```cpp
EASYPB_{TYPE}_EXTRA_FIELDS
EASYPB_{TYPE}_EXTRA_ENCODING(pb, message)
EASYPB_{TYPE}_EXTRA_DECODING(pb, message)
EASYPB_{TYPE}_EXTRA_POST_DECODING(pb, message)
```

`pb` is the current `easypb::Encoder` or `easypb::Decoder`, and `message` is the message object.
For example:

```cpp
#define EASYPB_Message_EXTRA_FIELDS \
    bool extra_flag = false;

#define EASYPB_Message_EXTRA_ENCODING(pb, message) \
    (pb).put_bool(100, (message).extra_flag);

#define EASYPB_Message_EXTRA_DECODING(pb, message) \
    case 100: (pb).get_bool(&(message).extra_flag); break;
```

Define insertion macros before including the generated header. See the [Tutorial](../examples/tutorial).
