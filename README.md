EasyProtoBuf is a single-header C++11 [ProtoBuf][] library that is
- easy to [learn](#motivating-example) - all field types are (de)serialized
with the uniform get_{FIELDTYPE} and put_{FIELDTYPE} calls
- easy to [use](#documentation) - you need to write only one line of code to (de)serialize each field
- easy to [grok and hack](include/easypb.hpp) - the entire library is only 666 LOC

Sorry, I fooled you... It's even easier!

[Codegen](codegen) translates .proto files into plain C++ structures
and generates encode/decode functions (de)serializing these structures into ProtoBuf format.
So, if you know how to use C++ structs, you just learned how to use EasyProtoBuf.
Scrap the docs, and have a nice beer! The rest is written for water lovers.


## Overview

Library features:
- encoding & decoding, i.e. get/put methods for all ProtoBuf field types
- string/bytes fields can be stored in any C++ type convertible from/to std::string_view (or easypb::string_view)
- repeated fields can be stored in any C++ container implementing push_back() and begin()/end()
- map fields can be stored in any C++ container similar enough to std::map
- not implemented: group wire format
- [protozero][] is a production-grade library with a similar API

[Codegen](codegen) features:
- generates C++ structure, encoder and decoder for each message type
- the generated decoder checks the presence of required fields in the decoded message
- cmdline options to tailor the generated code
- planned:
  - support of enum/oneof/map fields and nested message type definitions (and thus dogfooding Codegen)
  - protoc plugin
  - validation of enum, integer and bool values by the generated code
  - per-field C++ type specification

Files:
- [easypb.hpp](include/easypb.hpp) - the entire library
- [Codegen](codegen) - generates C++ structures and (de)coders from .pbs (compiled .proto) files
- [Tutorial](examples/tutorial) - learn how to use the library
- [Decoder](examples/decoder) - schema-less decoder of arbitrary ProtoBuf messages

Portability:
- we target compatibility with any C++11 compiler providing int32_t and int64_t types,
in particular gcc 4.7+ and clang 3.1+
- now we support only little-endian and big-endian CPUs with runtime detection,
but it can be improved to support other CPUs and compile-time detection
- in principle, the library can be ported to C++98 with 3rd-party replacement of `<cstdint>`

CI: while the final goal is to support any C++11 compiler, so far we tested only:
- Linux: gcc 4.7..14 and clang 3.5, 3.8, 7..18 on Ubuntu (x64);
plus default gcc compilers on Ubuntu LTS 14.04..24.04, Debian 10..12 and CentOS/RockyLinux 7..9
- Mac: clang 13..15 on Mac OS 11..13 (x64) and Mac OS 14 (ARM64), plus gcc 13 on MacOS 14
- Windows: only MSVC in x64 and x86 modes (the latter is the only 32-bit build in our tests)
- C++11 and C++17 modes for modern compilers (MSVC in C++14/17 modes)
- big-endian cpus: the support is implemented, but has not been tested so far
- planned: copy the CI scripts from [protozero][] and [xxHash][]
which tests a lot of older compilers and non-x86 platforms

Implemented so far:
- 100% of the library
- 66% of the Codegen
- 50% of the documentation (need exhaustive docs on the API and Codegen)
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
}
```

... [Codegen](codegen) generates the following C++ structure...
```cpp
struct Person
{
    std::string name = "AnnA";
    double weight = 0;
    std::vector<int32_t> numbers;
...
};
```

... that follows the official ProtoBuf [guidelines][] on the ProtoBuf->C++ type mapping,
while enclosing repeated types into `std::vector`.

And on top of that, [Codegen](codegen) generates two functions
that encode/decode Person in the ProtoBuf wire format:
```cpp
// Encode Person into a string buffer
std::string protobuf_msg = easypb::encode(person);

// Decode Person from a string buffer
Person person2 = easypb::decode<Person>(protobuf_msg);
```

And that's all you need to know to start using the library.
Check technical details in [Tutorial](examples/tutorial).



## Using the API

Even if you are going to implement your own encoder or decoder,
we recommend using [Codegen](codegen) to get a blueprint for your code.
For Person (see above), the generated code is:
```cpp
void Person::encode(easypb::Encoder &pb) const
{
    pb.put_string(1, name);
    pb.put_double(2, weight);
    pb.put_repeated_int32(3, ids);
}

void Person::decode(easypb::Decoder pb)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&name); break;
            case 2: pb.get_double(&weight); break;
            case 3: pb.get_repeated_int32(&ids); break;
            default: pb.skip_field();
        }
    }
}
```

So, the API consists of the following class methods
(where FTYPE is the Protobuf type of the field, e.g. 'fixed32' or 'message'):
- get_FTYPE reads the value of the non-repeated field
- get_repeated_FTYPE reads the value of the repeated field
- put_FTYPE writes the value of the non-repeated field
- put_repeated_FTYPE writes the value of the unpacked repeated field
- put_packed_FTYPE writes the value of the packed repeated field

The field number is the first parameter in put_* calls,
and placed in the case label before get_* calls.

You can use the returned value of get_FTYPE method instead of passing the variable address,
e.g. `weight = pb.get_double()`.

`get_FTYPE(&var)` accepts an optional second parameter - a pointer to a bool variable,
e.g. `pb.get_string(&name, &has_name)`.
This extra variable is set to `true` after the modification of `var`,
allowing the program to check which fields were actually present in the decoded message.
This form of `get_FTYPE` is employed in the code generated by Codegen,
both for required and optional fields.



# Documentation

EasyProtoBuf is a single-header library.
In order to use it, include [easypb.hpp](include/easypb.hpp).

All exceptions explicitly thrown by the library are derived
from easypb::exception. It may also throw std::bad_alloc
due to buffer management.


## Encoding API

Start encoding with the creation of the Encoder object:
```cpp
    easypb::Encoder pb;
```

Then proceed with encoding all present fields of the message:
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
- `put_packed_FTYPE`, is similar to `put_repeated_FTYPE`,
but encodes data in the [packed][] format.
- `put_map_FTYPE1_FTYPE2`, e.g. `put_map_string_int32` serializes the [map type][] `map<string, int32>`.
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
or a user can supply his own type as EASYPB_STRING_VIEW preprocessor macro,
e.g. define it to std::string.

Sub-messages and packed repeated fields always use 5-byte length prefix
(it can make encoded messages a bit longer than with other Protobuf libraries).

Compared to the [official][updating] ProtoBuf library,
EasyProtoBuf allows more flexibility in modifying the field type without losing the decoding compatibility.
You can make any changes to the field type as long as it stays inside the same "type domain":
- FP domain - only float and double
- zigzag domain - includes sint32 and sint64
- bytearray domain - strings, bytes and sub-messages
- integrals domain - all remaining scalar types (enum, bool, `int*`, `uint*`)
- aside from that, fixed-width integral fields are compatible with both integral and zigzag domain
- allows to switch between I32, I64 and VARINT representations for the same field as far as field type kept inside the same domain
- note that when changing the field type, values will be decoded correctly only if they fit into the range of both old and new field types - for integral types; while precision will be truncated to 32 bits - for FP types


# Motivation

It starts with the story of my FreeArc archiver:
- the first FreeArc version was implemented in Haskell, which is a very high-level language
- the second version was reimplemented in C++, both to increase performance and to increase the potential contributors' audience
- then, I realized that 80% of the archiver code (e.g. cmdline parsing) doesn't need C++ efficiency
and rewrote this part in Lua to simplify the code and further increase the potential contributors' audience
(the popularity of Haskell, C++ and Lua among non-professional programmers is at the proportion of 1:10:100)
- and, finally, I thought that the C++ part could be considered as a low-level core archiver library (AKA backend)
while the scripting part is a client implementing concrete frontend (cmdline, UI) on the top of the core.
The backend API provides only a few functions (e.g. compress and decompress) with LOTS of parameters.

And the best way to pass a lot of parameters to a C++ function is a plain C struct.
Using a serialization library to pass such a struct between languages greatly simplifies
adding bindings to the core API for new languages, such as Python, JavaScript, and so on.
So I decided to provide the backend API as a few functions accepting serialized data structures
for all their parameters.

At this moment, I started to research various popular serialization libraries
and finally chose the ProtoBuf format:
- FlatBuffers doesn't suppose deserialization, while I prefer to work with plain C++ structures
- MessagePack format is more self-describing (schema-less) than ProtoBuf, making it less efficient for schema-based serialization
- ProtoBuf format is the simplest one among all popular libs, although sometimes it's TOO simple
(e.g. maps are emulated via repeated pairs)
- Given its simplicity, it's no surprise that ProtoBuf is the most popular serialization format around,
with bindings implemented for more languages. And even if some exotic language misses a binding,
it would be easier to implement it for ProtoBuf than for any other serialization format.

So, I started to look around, but the tiniest C++ ProtoBuf library I found was still a whopping 4 KLOC
(while it neither supports maps nor provides a bindings generator).
This made me crazy - the entire ProtoBuf format is just 5 field types, what do you do in those kilolines of code?

You guessed it right - I decided to write my own ProtoBuf library (with maps and codegen, you know).
The first Decoder version was about 100 LOC and today the entire library is still only 666 LOC,
encoding and decoding all ProtoBuf types including maps.
Nevertheless, the [library][protozero] I rejected eventually provided many insights,
from API to internal organization, so it may be called the father of EasyProtoBuf.

[ProtoBuf]: https://developers.google.com/protocol-buffers
[guidelines]: https://protobuf.dev/programming-guides/proto3/#scalar
[field number]: https://protobuf.dev/programming-guides/proto3/#assigning
[packed]: https://protobuf.dev/programming-guides/encoding/#packed
[map type]: https://protobuf.dev/programming-guides/proto3/#maps
[updating]: https://protobuf.dev/programming-guides/proto3/#updating
[protozero]: https://github.com/mapbox/protozero
[xxHash]: https://github.com/Cyan4973/xxHash
