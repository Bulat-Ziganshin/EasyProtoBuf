EasyProtoBuf is a single-header C++11 [ProtoBuf][] library that is
- easy to [learn](#motivating-example) - all field types are (de)serialized
with the uniform get_{FIELDTYPE} and put_{FIELDTYPE} calls
- easy to [use](#documentation) - you need to write only one line of code to (de)serialize each field
- easy to [grok and hack](include/easypb.hpp) - the entire library is only 666 LOC

Sorry, I fooled you... It's even easier!

[Codegen](codegen) parses .proto files and translates them into plain C++ structures
and generates encode/decode functions that (de)serialize these structures into the ProtoBuf format.
So, if you know how to use C++ structs, you have just learned how to use EasyProtoBuf.
Scrap the docs, and have a nice beer! The rest is written for water lovers.


## Overview

Library features:
- encoding & decoding, i.e. get/put methods for all ProtoBuf field types
- string/bytes fields can be stored in any C++ type convertible to/from std::string_view (or easypb::string_view)
- repeated fields can be stored in any C++ container implementing push_back() and begin()/end()
- map fields can be stored in any C++ container similar enough to std::map
- limited `group` support: unknown groups are skipped during decoding, but declaring and encoding groups is not supported
- [protozero][] is a production-grade library with a similar API

[Codegen](codegen) features:
- generates C++ structures/enums and `encode`/`decode` implementations for top-level and nested types
- supports map fields with scalar, enum and message values, including nested message values
- the generated decoder checks the presence of required fields in the decoded message
- command-line options to tailor the generated code
- planned:
  - support for oneof fields
  - protoc plugin
  - validation of enum, integer and bool values by the generated code
  - per-field C++ type specification

Files:
- [easypb.hpp](include/easypb.hpp) - the entire library
- [Codegen](codegen) - generates C++ structures and (de)coders from `.proto` source files or `.pbs` descriptor sets
- [Tutorial](examples/tutorial) - learn how to use the library
- [Decoder](examples/decoder) - schema-less decoder of arbitrary ProtoBuf messages
- [File-tree benchmark](examples/filetree) - demonstrates around 600 MB/s serialization and deserialization throughput

Portability:
- we target compatibility with any C++11 compiler, in particular gcc 4.7+, clang 3.1+, and msvc 2013+
- now we support only little-endian and big-endian CPUs with runtime detection,
but it can be improved to support other CPUs and compile-time detection

CI: while the final goal is to support any C++11 compiler, so far we have tested only:
- Linux: gcc 4.7..14 and clang 3.5, 3.8, 7..18 on Ubuntu (x64);
plus default gcc compilers on Ubuntu LTS 14.04..24.04, Debian 10..12 and CentOS/RockyLinux 7..9
- macOS: clang 13..15 on macOS 11..13 (x64) and macOS 14 (ARM64), plus gcc 13 on macOS 14
- Windows: only MSVC in x64 and x86 modes (the latter is the only 32-bit build in our tests)
- C++11 and C++17 modes for modern compilers (MSVC in C++14/17 modes)
- big-endian CPUs: support is implemented, but has not been tested so far
- planned: copy the CI scripts from [protozero][] and [xxHash][]
which test many older compilers and non-x86 platforms

Implemented so far:
- 100% of the library
- 66% of the Codegen
- 50% of the documentation (more exhaustive documentation is needed for the API and Codegen)
- 25% of CI (ideally it should test every C++11 compiler on the Earth with every combination of compiler flags)
- 0% of the tests (the grand plan is to copy the exhaustive [protozero][] test suite)


## Motivating example

From this ProtoBuf message definition...
```proto
message Person
{
    required string name    = 1 [default = "AnnA"];
    optional double weight  = 2;
    repeated int32  numbers = 3;
    map<fixed64,string> labels = 4;
}
```

... [Codegen](codegen) generates the following C++ structure...
```cpp
struct Person
{
    std::string name = "AnnA";
    double weight = 0;
    std::vector<int32_t> numbers;
    std::map<uint64_t,std::string> labels;
...
};
```

... that follows the official ProtoBuf [guidelines][] on mapping ProtoBuf types to C++ types,
while enclosing repeated types in `std::vector` and maps in `std::map`.
The generated C++ type itself does not depend on EasyProtoBuf.

And on top of that, [Codegen](codegen) generates two functions
that encode/decode Person in the ProtoBuf wire format:
```cpp
// Encode Person into a string buffer
std::string protobuf_msg = easypb::encode(person);

// Decode Person from a string buffer
Person person2 = easypb::decode<Person>(protobuf_msg);
```

And that's all you need to know to start using the library.
See the technical details in the [Tutorial](examples/tutorial).



## Using the API

Even if you are going to implement your own encoder or decoder,
we recommend using [Codegen](codegen) to get a blueprint for your code.
For Person (see above), the generated code is:
```cpp
void encode(easypb::Encoder &pb, const Person &x)
{
    pb.put_string(1, x.name);
    pb.put_double(2, x.weight);
    pb.put_repeated_int32(3, x.numbers);
    pb.put_map_fixed64_string(4, x.labels);
}

void decode(easypb::Decoder pb, Person &x)
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

`encode` receives the output encoder by reference and the source object by const reference.
`decode` receives a cheap, non-owning decoder cursor by value and fills the destination object by reference.

EasyProtoBuf calls these functions without namespace qualification and relies on
[argument-dependent lookup](https://en.cppreference.com/w/cpp/language/adl).
Normally, place the overloads in the same namespace as the C++ type:
```cpp
namespace app
{
    struct Person;

    void encode(easypb::Encoder&, const Person&);
    void decode(easypb::Decoder, Person&);
}
```

For a type from a namespace that cannot be modified, the adapter overloads may instead be placed in `easypb`;
the `Encoder` or `Decoder` argument makes that namespace participate in argument-dependent lookup.
Do not add EasyProtoBuf overloads to `std`.
A type with private state can expose accessors or declare the overloads as friends.

The buffer-level API creates the encoder/decoder and invokes the overloads:
```cpp
std::string protobuf_msg = easypb::encode(person);
Person person2 = easypb::decode<Person>(protobuf_msg);
```

`easypb::decode<T>()` value-initializes `T`, so `T` must be default-constructible.
Nested and repeated messages have the same requirement.

Codegen emits the overloads as `inline` functions, so generated code can be included in multiple translation units.

The field API consists of the following `Encoder` and `Decoder` methods
(where FTYPE is the Protobuf type of the field, e.g. `fixed32` or `message`):
- `get_FTYPE` reads a non-repeated field
- `get_repeated_FTYPE` reads a repeated field
- `get_map_FTYPE1_FTYPE2` reads one map entry and inserts it into the supplied C++ map container
- `put_FTYPE` writes a non-repeated field
- `put_repeated_FTYPE` writes an unpacked repeated field
- `put_packed_FTYPE` writes a packed repeated field
- `put_map_FTYPE1_FTYPE2` writes all entries from the supplied C++ map container

The field number is the first parameter in `put_*` calls,
and is placed in the `case` label before `get_*` calls.

You can use the value returned by `get_FTYPE()` instead of passing a variable address,
e.g. `x.weight = pb.get_double()`.

`get_FTYPE(&var)` accepts an optional second parameter pointing to a bool,
e.g. `pb.get_string(&x.name, &x.has_name)`.
It is set to `true` after `var` has been modified, allowing the program to record which fields were present.
Codegen uses this form for required and optional fields.


# Documentation

EasyProtoBuf is a single-header library.
In order to use it, include [easypb.hpp](include/easypb.hpp).

All exceptions explicitly thrown by the library are derived
from easypb::exception. It may also throw std::bad_alloc
due to buffer management.


## Encoding API

Start encoding by creating an Encoder object:
```cpp
    easypb::Encoder pb;
```

Then proceed by encoding all fields present in the message:
```cpp
    pb.put_string(1, name);
    pb.put_double(2, weight);
    pb.put_repeated_int32(3, ids);
```

Finally, retrieve the encoded message from the Encoder object:
```cpp
    std::string protobuf_msg = pb.result();
```

This call clears the contents of the Encoder, so it can be reused to encode more messages.

The first parameter of any `put_*` call is the [field number][],
and the second parameter is the value to encode.

There are several groups of `put_*` methods:
- `put_FTYPE`, e.g. `put_string`, encodes a single value.
- `put_repeated_FTYPE`, encodes multiple values in one call.
The second parameter should be an iterable container.
- `put_packed_FTYPE` is similar to `put_repeated_FTYPE`,
but encodes data in the [packed][] format.
- `put_map_FTYPE1_FTYPE2`, e.g. `put_map_string_int32`, serializes the [map type][] `map<string, int32>`.
The second parameter should be a compatible C++ map container,
e.g. `std::map<std::string, int32_t>`.

`FTYPE` here should be replaced by the ProtoBuf field type
of the corresponding message field, e.g. `int32`, `bytes` and so on,
except that for any message type we use the fixed string `message`.


## Decoding API

The Decoder keeps only the raw pointer to the buffer passed to the constructor.
Thus, the buffer should neither be freed nor moved until decoding is complete.


## Code generator

The code generator is described in the separate [documentation](codegen/README.md).


## Boring details

Despite its simplicity, the library is quite fast,
thanks to the use of std::string_view (e.g. avoiding large buffer copies)
and efficient read_varint/write_varint implementation.

On pre-C++17 compilers, the library uses its own
implementation of string_view to ensure good performance,
or users can supply their own type via the EASYPB_STRING_VIEW preprocessor macro,
e.g. define it to std::string.

Sub-messages and packed repeated fields always use a 5-byte length prefix
(it can make encoded messages a bit longer than with other Protobuf libraries).

EasyProtoBuf currently differs from the official ProtoBuf decoding semantics
in several ways:

- generated proto2 `required` checks currently run at the end of every generated
  `decode` call instead of once after the complete message has been merged and
  recursively validated. A singular embedded message whose required fields are
  split across multiple wire occurrences can therefore be rejected too early.
  The same issue affects multiple message-valued field 2 occurrences inside one
  map entry. Presence tracking through `has_*` is not itself the problem;
- conversely, an omitted message value in a map entry creates a default-initialized
  mapped object without recursively checking its required fields;
- a missing proto2 enum map value is value-initialized to zero rather than using
  the enum's proto2 default (its first declared value), and closed-enum
  validation semantics are not enforced;
- unknown fields in ordinary messages, including message values stored in maps,
  are discarded rather than retained for later serialization.

Compared with the [official][updating] ProtoBuf library, EasyProtoBuf also
allows more flexibility in modifying the field type without losing the decoding compatibility.
You can make any changes to the field type as long as it stays inside the same "type domain":
- FP domain - only float and double
- zigzag domain - includes sint32 and sint64
- bytearray domain - strings, bytes and sub-messages
- integrals domain - all remaining scalar types (enum, bool, `int*`, `uint*`)
- aside from that, fixed-width integral fields are compatible with both the integral and zigzag domains
- allows switching between I32, I64 and VARINT representations for the same field as long as the field type stays within the same domain
- note that when changing the field type, values will be decoded correctly only if they fit into the range of both the old and new field types for integral types, whereas precision will be truncated to 32 bits for FP types


# Motivation

It starts with the story of my FreeArc archiver:
- the first FreeArc version was implemented in Haskell, which is a very high-level language
- the second version was reimplemented in C++, both to increase performance and to broaden its potential contributor base
- then, I realized that 80% of the archiver code (e.g. cmdline parsing) doesn't need C++ efficiency
and rewrote this part in Lua to simplify the code and further broaden the potential contributor base
(the relative popularity of Haskell, C++, and Lua among non-professional programmers is roughly 1:10:100)
- and, finally, I thought that the C++ part could be considered a low-level core archiver library (AKA backend)
while the scripting part is a client implementing a concrete frontend (command line, UI) on top of the core.
The backend API provides only a few functions (e.g. compress and decompress) with LOTS of parameters.

And the best way to pass a lot of parameters to a C++ function is a plain C struct.
Using a serialization library to pass such a struct between languages greatly simplifies
adding bindings to the core API for new languages, such as Python, JavaScript, and so on.
So I decided to provide the backend API as a few functions that accept their parameters
as serialized data structures.

At that point, I started researching various popular serialization libraries
and finally chose the ProtoBuf format:
- FlatBuffers doesn't support deserialization, while I prefer to work with plain C++ structures
- MessagePack format is more self-describing (schema-less) than ProtoBuf, making it less efficient for schema-based serialization
- The ProtoBuf format is the simplest of all popular serialization formats, although sometimes it's TOO simple
(e.g. maps are emulated via repeated pairs)
- Given its simplicity, it's no surprise that ProtoBuf is the most popular serialization format around,
with bindings implemented for many languages. And even if some exotic language lacks a binding,
it would be easier to implement it for ProtoBuf than for any other serialization format.

So, I started to look around, but the tiniest C++ ProtoBuf library I found was still a whopping 4 KLOC
(while it neither supports maps nor provides a binding generator).
This made me crazy - the entire ProtoBuf format has just 5 field types; what do you do in all those thousands of lines of code?

You guessed it - I decided to write my own ProtoBuf library (with maps and codegen, you know).
The first Decoder version was about 100 LOC, and today the entire library is still only 666 LOC,
encoding and decoding all ProtoBuf types including maps.
Nevertheless, although I ultimately decided not to use the [Protozero library][protozero],
it gave me many valuable insights, from API to internal organization,
so it may be called the father of EasyProtoBuf.

[ProtoBuf]: https://developers.google.com/protocol-buffers
[guidelines]: https://protobuf.dev/programming-guides/proto3/#scalar
[field number]: https://protobuf.dev/programming-guides/proto3/#assigning
[packed]: https://protobuf.dev/programming-guides/encoding/#packed
[map type]: https://protobuf.dev/programming-guides/proto3/#maps
[updating]: https://protobuf.dev/programming-guides/proto3/#updating
[protozero]: https://github.com/mapbox/protozero
[xxHash]: https://github.com/Cyan4973/xxHash
