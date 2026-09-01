# EasyProtoBuf Tutorial

This tutorial introduces the runtime API. The C++ snippets assume that `<easypb.hpp>` and the required
standard-library headers have been included and that `using namespace easypb;` is in effect.

## Direct use of `Encoder` and `Decoder`

As a first example, consider this data structure:

```proto
syntax = "proto3";

message Persona {
  string name = 1;
  repeated int32 values = 2;
  map<string, double> properties = 3;
}
```

Use an `Encoder` to encode the data:

```cpp
const std::string name = "Anna";
const std::vector<int32_t> values = {17, 42};
const std::map<std::string, double> properties = {{"pi", 3.14}, {"e", 2.71}};

Encoder encoder;
encoder.put_string(1, name);
encoder.put_repeated_int32(2, values);
encoder.put_map_string_double(3, properties);
std::string wire = encoder.result();
```

Each logical field is encoded with a single call to the corresponding `put_*` function.
A single call may write an entire repeated field or map.
Here `put_repeated_int32` emits unpacked occurrences; use `put_packed_int32` when packed output is desired.

Now read the same fields:

```cpp
Decoder decoder(wire);
std::string name;
std::vector<int32_t> values;
std::map<std::string, double> properties;

while (decoder.get_next_field()) {
    switch (decoder.field_num) {
        case 1: decoder.get_string(&name); break;
        case 2: decoder.get_repeated_int32(&values); break;
        case 3: decoder.get_map_string_double(&properties); break;
        default: decoder.skip_field();
    }
}
```

Reading each known field also takes one matching `get_*` call. That call lives inside a field loop, however, and
may execute multiple times or not at all. Every field returned by `get_next_field()` must be consumed exactly
once, either by its `get_*` operation or by `skip_field()`.

## `encode` and `decode` functions

Usually, we create a `struct` or `class` for each message type and define `encode` and `decode` overloads for its
serialization and deserialization. Put these overloads in the same namespace as `Persona` or in the `easypb` namespace:

```cpp
struct Persona
{
    std::string name;
    std::vector<int32_t> values;
    std::map<std::string, double> properties;
};

inline void encode(Encoder& pb, const Persona& value)
{
    pb.put_string(1, value.name);
    pb.put_repeated_int32(2, value.values);
    pb.put_map_string_double(3, value.properties);
}

inline void decode(Decoder pb, Persona& value)
{
    while (pb.get_next_field()) {
        switch (pb.field_num) {
            case 1: pb.get_string(&value.name); break;
            case 2: pb.get_repeated_int32(&value.values); break;
            case 3: pb.get_map_string_double(&value.properties); break;
            default: pb.skip_field();
        }
    }
}
```

After that, only a few operations remain at the call site:

```cpp
Persona me;
// Initialize me as needed.

Encoder encoder;
encode(encoder, me);
std::string wire = encoder.result();

Decoder decoder(wire);
Persona my_clone;
decode(decoder, my_clone);
```

The library provides small helpers that perform exactly these steps:

```cpp
std::string wire = easypb::encode(me);
Persona my_clone = easypb::decode<Persona>(wire);
```

## Handling message hierarchies

The more interesting part begins when we build a hierarchy of objects. For example, add another message type
that uses `Persona`:

```proto
message SocialCircle {
  Persona spouse = 1;
  repeated Persona friends = 2;
  map<string, Persona> relatives = 3;
}
```

Its C++ definitions follow the same pattern:

```cpp
struct SocialCircle
{
    Persona spouse;
    std::vector<Persona> friends;
    std::map<std::string, Persona> relatives;
};

inline void encode(Encoder& pb, const SocialCircle& value)
{
    pb.put_message(1, value.spouse);
    pb.put_repeated_message(2, value.friends);
    pb.put_map_string_message(3, value.relatives);
}

inline void decode(Decoder pb, SocialCircle& value)
{
    while (pb.get_next_field()) {
        switch (pb.field_num) {
            case 1: pb.get_message(&value.spouse); break;
            case 2: pb.get_repeated_message(&value.friends); break;
            case 3: pb.get_map_string_message(&value.relatives); break;
            default: pb.skip_field();
        }
    }
}
```

We can now serialize and deserialize an entire `SocialCircle` with one call in either direction:

```cpp
SocialCircle circle;
// Populate circle as needed.

std::string wire = easypb::encode(circle);
SocialCircle restored = easypb::decode<SocialCircle>(wire);
```

All nested `Persona` objects are processed automatically. Operations such as `put_message`,
`get_repeated_message`, and `put_map_string_message` call the corresponding global `encode` or `decode`
customization function for each nested object. The compiler selects the matching overload, recursively processing
the entire object hierarchy, including messages stored in sequential containers and maps. This works with any C++
message type for which suitable `encode` and `decode` overloads have been defined.

## `PBType` and generic `get`/`put` operations

You have probably noticed the naming pattern of the `get_*` and `put_*` operations. The API provides seven
operation families, each available as a generic template and through type-specific named aliases:

| Direction | Singular field | Repeated, unpacked | Repeated, packed | Map |
|---|---|---|---|---|
| Encoding | put_TYPE<br>put<PB_TYPE> | put_repeated_TYPE<br>put_repeated<PB_TYPE> | put_packed_TYPE<br>put_packed<PB_TYPE> | put_map_KEY_VALUE<br>put_map<PB_KEY, PB_VALUE> |
| Decoding | get_TYPE<br>get<PB_TYPE> | get_repeated_TYPE<br>get_repeated<PB_TYPE> | — | get_map_KEY_VALUE<br>get_map<PB_KEY, PB_VALUE> |

There is no separate `get_packed` family: `get_repeated` accepts both packed and unpacked occurrences
of packable scalar types.

`TYPE`, `KEY`, and `VALUE` are replaced by one of the 17 Protobuf type names. The suffix `message` represents an
application-defined message type, while `enum` represents an application enum. Generic forms select these types
with values of `easypb::PBType`:

| `.proto` type / alias suffix | `PBType` | Canonical C++ type | Packable | Map key |
|---|---|---|---|---|
| int32 | PB_INT32 | int32_t | yes | yes |
| int64 | PB_INT64 | int64_t | yes | yes |
| uint32 | PB_UINT32 | uint32_t | yes | yes |
| uint64 | PB_UINT64 | uint64_t | yes | yes |
| sint32 | PB_SINT32 | int32_t | yes | yes |
| sint64 | PB_SINT64 | int64_t | yes | yes |
| fixed32 | PB_FIXED32 | uint32_t | yes | yes |
| fixed64 | PB_FIXED64 | uint64_t | yes | yes |
| sfixed32 | PB_SFIXED32 | int32_t | yes | yes |
| sfixed64 | PB_SFIXED64 | int64_t | yes | yes |
| bool | PB_BOOL | bool | yes | yes |
| enum | PB_ENUM | int32_t / enum | yes | no |
| float | PB_FLOAT | float | yes | no |
| double | PB_DOUBLE | double | yes | no |
| string | PB_STRING | string-like | no | yes |
| bytes | PB_BYTES | string-like | no | no |
| message | PB_MESSAGE | custom C++ type | no | no |

For example, `put_float` is an alias for `put<PB_FLOAT>`, while `put_map_string_message` is an alias for
`put_map<PB_STRING, PB_MESSAGE>`.

The selected `PBType`, rather than the source or destination C++ type, determines the Protobuf conversion.
Generic operations are especially useful when writing reusable code that works with different Protobuf types.

## Sequential containers and maps

The library uses duck typing for sequential containers and maps: a type is accepted if it supports the operations
required by the selected `get`/`put` operation. A container need not be a particular standard-library type:

| Operation | Required interface |
|---|---|
| put_repeated / put_packed | Range-based iteration; each element must be accepted by the selected scalar or message writer. |
| get_repeated | A nested `value_type` and `push_back(value)`; the value type must be constructible from the decoded value. |
| get_repeated<PB_MESSAGE> | The preceding requirements plus default construction of `value_type` and a matching `decode` overload. |
| put_map | Range-based iteration over entries exposing `.first` and `.second`. |
| get_map | Nested `key_type` and `mapped_type`, default construction of both, and `operator[]` followed by assignment of the decoded value. |

Encoding does not require `size()`, random access, or contiguous storage. Repeated decoding appends to the
destination container, and map decoding inserts or replaces the entry selected by its decoded key; neither
operation clears the destination first.

The following table lists compatible containers from the C++26 standard library and Boost 1.92:

| | get | put |
|---|---|---|
| C++26 | **Repeated:** std::vector, std::deque, std::list, std::inplace_vector, std::basic_string <br><br> **Maps:** std::map, std::unordered_map, std::flat_map | **Repeated:** std::vector, std::deque, std::list, std::inplace_vector, std::basic_string, std::array, std::forward_list, std::hive, std::set, std::multiset, std::unordered_set, std::unordered_multiset, std::flat_set, std::flat_multiset <br> <br> **Maps:** std::map, std::unordered_map, std::flat_map, std::multimap, std::unordered_multimap, std::flat_multimap, <br> and std::vector, std::list, or std::array of key/value pairs |
| Boost 1.92 | **Repeated:** <br>boost::container::{vector, deque, list, stable_vector, small_vector, static_vector, devector, segtor, basic_string} <br><br> boost::{circular_buffer, circular_buffer_space_optimized, static_strings::basic_static_string} <br><br> **Maps:** <br>boost::container::{map, flat_map} <br><br> boost::{unordered_map, unordered_flat_map, unordered_node_map} | **Repeated:** <br> boost::container::{vector, deque, list, stable_vector, small_vector, static_vector, devector, segtor, basic_string, slist, hub} <br><br> boost::{array, circular_buffer, circular_buffer_space_optimized, static_strings::basic_static_string} <br><br> **Maps:** <br> boost::container::{map, flat_map, multimap, flat_multimap} <br><br> boost::{unordered_map, unordered_flat_map, unordered_node_map, unordered_multimap} |

The Protobuf schema still restricts map keys to integral types, `bool`, and `string`, as indicated in the
`PBType` table. A map value may use any of the 17 types, including `enum`, `bytes`, and `message`.

## Buffer ownership and lifetime

`Encoder` owns the `std::string` into which it writes the output data. It is therefore normally passed by
reference. `Encoder::result()` transfers the current buffer to the caller and replaces it with a new empty one.

`Decoder` is a lightweight, non-owning cursor: it stores two pointers into the encoded buffer plus
metadata for the current field. It is therefore normally passed by value. Decoder copies have independent cursor
positions, but they still refer to the same input storage. The buffer supplied to a decoder must remain alive and
must not be moved or resized until decoding has finished.

The `get_string()` and `get_bytes()` functions return an `easypb::string_view` into the original input buffer.
Such views remain valid only as long as the buffer remains alive and unmoved.
Constructing an owning `std::string` copies the decoded data and eliminates that lifetime dependency:

```cpp
easypb::string_view borrowed_text = pb.get_string();
std::string owned_bytes = std::string(pb.get_bytes());
```
