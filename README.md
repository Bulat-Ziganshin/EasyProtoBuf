EasyProtoBuf is a single-header C++11 [ProtoBuf](https://developers.google.com/protocol-buffers) library that is
- easy to [learn](#motivating-example) - all field types are (de)serialized
with the uniform get_{FIELDTYPE} and put_{FIELDTYPE} calls
- easy to [use](#documentation) - you need just one line of code to (de)serialize each field
- easy to [grok and hack](include/easypb.hpp) - the entire library is only 600 LOC

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
- little-endian and big-endian CPUs support
- not implemented: group wire format
- [protozero](https://github.com/mapbox/protozero) is a production-grade library with a very similar API

[Codegen](codegen) features currently implemented and planned:
- [x] generates C++ structure, encoder and decoder for each message type
- [x] the generated decoder checks the presence of required fields in the decoded message
- [x] cmdline options to tailor the generated code
- [ ] per-field C++ type specification via field option
- [ ] support of enum/oneof/map fields and nested message type definitions (and thus dogfooding Codegen)
- [ ] validation of enum, integer and bool values by the generated code
- [ ] protoc plugin
- [ ] C++11 compatibility (now it requires C++17 compiler, unlike the library itself)

Files:
- [easypb.hpp](include/easypb.hpp) - the entire library
- [Codegen](codegen) - generates C++ structures and (de)coders from .pbs (compiled .proto) files
- [Tutorial](examples/tutorial) - learn how to use the library
- [Decoder](examples/decoder) - schema-less decoder of arbitrary ProtoBuf messages

Portability. While the final goal is to support any C++11 compiler, so far we tested only:
- Linux: tested gcc 9..13 and clang 10..15 on Ubuntu 20.04/22.04 (x64)
- Mac: tested clang 13..15 on Mac OS 11..13 (x64) and Mac OS 14 (ARM64).
gcc doesn't work on Mac OS platforms and I don't know why.
- Windows: tested only CL in x64 and x86 modes (the latter is the only 32-bit build that we tested so far)
- C++11: tested locally, but all the tests above were made only in C++17 mode (since Codegen requires C++17 ATM)
- big-endian cpus: the support is implemented, but has not been tested so far
- planned: copy the CI scripts from [protozero](https://github.com/mapbox/protozero)
which tests a lot of older compilers

Implemented so far:
- 100% of the library
- 66% of the Codegen
- 50% of the documentation
- 25% of CI (testing on various platforms with various compiler versions)
- 0% of the tests


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

... that follows the official ProtoBuf
[guidelines](https://protobuf.dev/programming-guides/proto3/#scalar)
on the ProtoBuf->C++ type mapping,
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
For Person, the generated code is:
```cpp
void Person::encode(easypb::Encoder &pb)
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

Encoding starts with the creation of the encoder object:
```cpp
    easypb::Encoder pb;
```

Then you proceed with encoding all present fields of the message:
```cpp
    pb.put_string(1, name);
    pb.put_double(2, weight);
    pb.put_repeated_int32(3, ids);
```

Finally, you extract the encoded message from the encoder object:
```cpp
    std::string protobuf_msg = pb.result();
```


## Decoding API

The Decoder keeps only the raw pointer to the buffer passed to the constructor.
Thus, the buffer should be neither freed nor moved till the decoding is finished.


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

Compared to the [official](https://protobuf.dev/programming-guides/proto3/#updating)
ProtoBuf library allows more flexibility in modifying the field type without losing the decoding compatibility.
You can make any changes to the field type as long as it stays inside the same "type domain":
- FP domain - only float and double
- zigzag domain - includes sint32 and sint64
- bytearray domain - strings, bytes and sub-messages
- integrals domain - all remaining scalar types (enum, bool, `int*`, `uint*`)
- aside from that, fixed-width integral fields are compatible with both integral and zigzag domain
- allows to switch between I32, I64 and VARINT representations for the same field as far as field type kept inside the same domain
- note that when changing the field type, values will be decoded correctly only if they fit into the range of both old and new field types - for integral types; while precision will be truncated to 32 bits - for FP types


## Motivation

It starts with the story of my FreeArc archiver:
- the first FreeArc version was implemented in Haskell, which is a very high-level language
- the second version was reimplemented in C++, both to increase performance and to increase the potential contributors' audience
- then, I realized that 80% of the archiver code (e.g. cmdline parsing) doesn't need C++ efficiency
and rewrote this part in Lua to simplify the code and further increase the potential contributors' audience
(the popularity of Haskell, C++ and Lua among programmers is at the proportion of 1:10:100)
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
- ProtoBuf format is the simplest one among all popular libs, although sometimes it's TOO simple
(e.g. maps are emulated via repeated pairs)
- FlatBuffers doesn't suppose deserialization, while I prefer to work with plain C++ structures
- MessagePack format is more self-describing (schema-less) than ProtoBuf, making it less efficient for schema-based serialization
- given its simplicity, it's no surprise that ProtoBuf is the most popular serialization format around,
with bindings implemented for more languages. And even if some exotic language misses a binding,
it would be easier to implement it for ProtoBuf than for any other serialization format.

Now, I looked around, but the tiniest C++ ProtoBuf library I found was still a whopping 4 KLOC
(while it neither supports maps nor provides a bindings generator).
This made me crazy - the entire ProtoBuf format is just 5 field types, what do you do in those kilolines of code?

You guessed it right - I decided to write my own ProtoBuf library (with maps and codegen, you know).
The first Decoder version was about 100 LOC and today the entire library is still only 600 LOC,
encoding and decoding all ProtoBuf types including maps.
Nevertheless, the [library](https://github.com/mapbox/protozero) I rejected
eventually provided many insights, from the API to internal organization,
so I can call it a father of my own lib.
