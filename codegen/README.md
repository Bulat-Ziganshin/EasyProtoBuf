# EasyProtoBuf Codegen

EasyProtoBuf Codegen turns Protocol Buffers schemas into lightweight C++ bindings for [EasyProtoBuf](../README.md). It generates a single header containing plain structures and enums together with free `inline encode(...)` and `decode(...)` overloads.

The generated data types do not inherit from runtime classes and do not require generated `.cpp` files. Applications use the small header-only EasyProtoBuf runtime instead of linking the official Protobuf runtime, making Codegen useful when a transparent C++ data model, a small dependency footprint, and C++11 portability matter.

## Capabilities

- Generate C++ from either `.proto` source or a binary descriptor set produced by `protoc`.
- Generate top-level and nested messages, enums, repeated fields, and maps with scalar, enum, or message values.
- Generate presence tracking, proto2 required-field checks, schema defaults, and packed repeated-field codecs.
- Customize string, repeated-field, and map C++ types, or generate codecs for existing C++ types.
- Optionally generate direct self-recursive repeated/map message containers with [`--allow-self-recursive-containers`](OPTIONS.md#structural-options).
- Build with the embedded `.proto` parser or as a smaller descriptor-set-only executable.

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

Depending on the selected options, Codegen can generate C++ code like this:

```cpp
struct Person
{
    std::string name = "AnnA";
    double weight = 0;
    std::vector<int32_t> numbers;
    std::map<uint64_t,std::string> labels;
};

inline void encode(easypb::Encoder &pb, const Person &x)
{
    pb.put_string(1, x.name);
    pb.put_double(2, x.weight);
    pb.put_repeated_int32(3, x.numbers);
    pb.put_map_fixed64_string(4, x.labels);
}

inline void decode(easypb::Decoder pb, Person &x)
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
- [Embedded `.proto` parser](parser/README.md) — parser API, lifetime rules, unresolved-import behavior, and parser-specific internals.
