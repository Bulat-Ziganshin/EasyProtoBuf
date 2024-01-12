A minimal C++ [ProtoBuf](https://developers.google.com/protocol-buffers) library that is
- easy to learn
- easy to use
- easy to grok and hack, the entire [library](include/easypb) is only 500 LOC
- adds minimal overhead to your executable
- includes [Codegen](codegen) that translates .proto files into plain C++ structures with ProtoBuf encoders/decoders


## Overview

Library features currently implemented and planned:
- [x] encoding & decoding, i.e. get/put methods for all ProtoBuf field types, except for maps
- [x] requires C++17, which may be lowered to C++11 by replacing uses of std::string_view with std::string
- [x] string/bytes fields can be stored in any C++ type convertible from/to std::string_view
- [x] repeated fields can be stored in any C++ container implementing push_back() and begin()/end()
- [x] big-endian CPUs support
- [ ] group wire format
- [protozero](https://github.com/mapbox/protozero) is a production-grade library with a very similar API

[Codegen](codegen) features currently implemented and planned:
- [x] generates C++ structure, encoder and decoder for each message type
- [x] the generated decoder checks presence of required fields in the decoded message
- [x] cmdline options to tailor the generated code
- [ ] per-field C++ type specification via field option
- [ ] support of enum/oneof/map fields and nested message type definitions (and thus dogfooding Codegen)
- [ ] validation of enum, integer and bool values by the generated code
- [ ] protoc plugin

Files:
- [encoder.hpp](include/easypb/encoder.hpp) - the encoding library
- [decoder.hpp](include/easypb/decoder.hpp) - the decoding library
- [Codegen](codegen) - generates C++ structures and (de)coders from .pbs (compiled .proto) files
- [Tutorial](examples/tutorial) - learn how to use the library
- [Decoder](examples/decoder) - schema-less decoder of arbitrary ProtoBuf messages



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
we recommend to use [Codegen](codegen) to get a blueprint for your code.
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
- get_FTYPE reads value of non-repeated field
- get_repeated_FTYPE reads value of repeated field
- put_FTYPE writes value of non-repeated field
- put_repeated_FTYPE writes value of unpacked repeated field
- put_packed_FTYPE writes value of packed repeated field

Field number is the first parameter in put_* calls,
and placed in the case label before get_* calls.

You can use the returned value of get_FTYPE method instead of passing the variable address,
e.g. `weight = pb.get_double()`.

`get_FTYPE(&var)` accepts an optional second parameter - a pointer to a bool variable,
e.g. `pb.get_string(&name, &has_name)`.
This extra variable is set to `true` after the modification of `var`,
allowing the program to check which fields were actually present in the decoded message.
This form of `get_FTYPE` is employed in the code generated by Codegen,
both for required and optional fields.



## Boring details

Despite its simplicity, the library is quite fast,
thanks to use of std::string_view (e.g. avoiding large buffer copies)
and efficient read_varint/write_varint implementation.

Sub-messages and packed repeated fields always use 5-byte length prefix
(it can make encoded messages a bit longer than with other Protobuf libraries).

All parsing errors (both in the library and generated code) are signalled using plain std::runtime_error.

Compared to the [official](https://protobuf.dev/programming-guides/proto3/#updating)
ProtoBuf library, it allows more flexibility in modifying the field type without losing the decoding compatibility.
You can make any changes to field type as far as it stays inside the same "type domain":
- FP domain - only float and double
- zigzag domain - includes sint32 and sint64
- bytearray domain - strings, bytes and sub-messages
- integrals domain - all remaining scalar types (enum, bool, `int*`, `uint*`)
- aside of that, fixed-width integral fields are compatible with both integral and zigzag domain
- allows to switch between I32, I64 and VARINT representations for the same field as far as field type keept inside the same domain
- note that when changing the field type, values will be decoded correctly only if they fit into the range of both old and new field type - for integral types; while precision will be truncated to 32 bits - for FP types
