# EasyProtoBuf Codegen

EasyProtoBuf Codegen turns Protocol Buffers schemas into lightweight C++ bindings for [EasyProtoBuf](../README.md). It generates a single header containing plain structures and enums together with free `inline encode(...)` and `decode(...)` overloads.

The generated data types do not inherit from runtime classes and do not require generated `.cpp` files. Applications use the small header-only EasyProtoBuf runtime instead of linking the official Protobuf runtime, making Codegen useful when a transparent C++ data model, a small dependency footprint, and C++11 portability matter.

## Capabilities

- Generate C++ from either `.proto` source or a binary descriptor set produced by `protoc`.
- Load imports from ordered search roots, link the whole set, and generate complete per-target headers with automatic dependency includes.
- Select descriptor-set targets explicitly and publish one header per target below `--out-dir`.
- Generate top-level and nested messages, enums, repeated fields, and maps with scalar, enum, or message values.
- Map Protobuf packages directly to C++ namespaces.
- Generate presence tracking, proto2 required-field checks, schema defaults, and packed repeated-field codecs.
- Customize C++ data types of generated message fields globally or per field, or generate codecs for existing C++ types.
- Optionally generate direct self-recursive repeated/map message containers with [`--allow-self-recursive-containers`](OPTIONS.md#structural-options).
- Build with the embedded `.proto` parser or as a smaller descriptor-set-only executable.

## Command-line contract

- `--help` always wins: `codegen --help --nope`, `codegen --help --groff` and `codegen --help --bash` print the normal help text and exit 0, while an unknown flag on its own is refused with a nonzero exit. A first `--` ends option parsing, so a later `--help` is an input spelling.
- The whole result of a successful run goes to stdout, and a failure leaves stdout empty. A redirected stdout uses LF line endings on every platform, so it is byte-identical to the same header written with `--out-dir`.
- `--print-descriptor` is inspection, not generation: it prints every file of a descriptor set, including imports, `reserved` declarations, extension ranges and the modeled message/enum options, and it never links or generates C++. The one semantic check it runs is the reference-free MessageSet rule: a message that declares `message_set_wire_format` and ordinary fields (or the option in a proto3 file) is refused with the generation path's sentence and exit 1 instead of being printed with exit 0.
- `--benchmark-parser` benchmarks `.proto` source only; descriptor-set input is refused.
- `@file` response files are not supported: a positional input that begins with `@` is refused with a message naming `@file` instead of being expanded. Pass the arguments directly, or let the build system expand them. This is a documented non-goal (owner decision D-05); see [Deliberate strictness and non-goals](OPTIONS.md#deliberate-strictness-and-non-goals).

See [Command-line options](OPTIONS.md) for the full rules.

## Basic use

Save this schema as `person.proto`:

```proto
message Person
{
    required string name    = 1 [default = "AnnA"];
    optional double weight  = 2;
    repeated int32  numbers = 3;
    map<fixed64,string> labels = 4;
}
```

Generate a C++ header:

```sh
codegen person.proto >person.pb.hpp
```

Schemas with imports name their search roots; several files are published together with `--out-dir`, and a complete descriptor set selects its targets explicitly:

```sh
codegen -I schemas --out-dir build/generated common/state.proto common/node.proto api/report.proto
codegen --descriptor-set project.pbs --target common/state.proto --target common/node.proto --target api/report.proto --out-dir build/generated
```

Generate every dependency of a selected target as well: a complete link includes the dependency's header, so the set must be closed for the generated C++ to compile.

Depending on the selected options, Codegen can generate C++ code like this:

```cpp
struct Person
{
    ::std::string name = "AnnA";
    double weight = 0;
    ::std::vector< ::int32_t> numbers;
    ::std::map< ::uint64_t,::std::string> labels;
};

inline void encode(::easypb::Encoder &pb, const Person &x)
{
    pb.put_string(1, x.name);
    pb.put_double(2, x.weight);
    pb.put_repeated_int32(3, x.numbers);
    pb.put_map_fixed64_string(4, x.labels);
}

inline void decode(::easypb::Decoder pb, Person &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name); break;
            case 2: pb.get_double(&x.weight); break;
            case 3: pb.get_repeated_int32(&x.numbers); break;
            case 4: pb.get_map_fixed64_string(&x.labels); break;
            default: pb.skip_field();
        }
    }
}
```

Include the generated header to use `Person` directly with EasyProtoBuf:

```cpp
#include <easypb.hpp>
#include "person.pb.hpp"
...

// Encode Person into a string buffer
std::string protobuf_msg = easypb::encode(person);

// Decode Person from a string buffer
Person person2 = easypb::decode<Person>(protobuf_msg);
```

## Documentation

Continue with the topic-specific guides for generated-code semantics, input modes, output customization, and implementation details:

- [Building and testing](BUILDING.md) — build Codegen, omit the `.proto` parser if desired, and run its tests.
- [Generated C++ code](GENERATED_CODE.md) — generated types/codecs, nesting, enums, maps, defaults, packed fields, insertion points, and limitations.
- [Command-line options](OPTIONS.md) — input modes, generated-code options, C++ container/type options, and parser utility modes.
- [Internals](INTERNALS.md) — implementation layout and generation pipeline for contributors.
- [Source loading and the import graph](LOADER.md) — how search roots, logical names, and the import graph fit together for contributors, and where the loader deliberately matches or exceeds the reference compiler.
- [Embedded `.proto` parser](parser/README.md) — parser API, lifetime rules, unresolved-import behavior, and parser-specific internals.
