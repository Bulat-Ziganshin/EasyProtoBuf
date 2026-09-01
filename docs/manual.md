# EasyProtoBuf

EasyProtoBuf is a single-header C++11 library for encoding and decoding the Protocol Buffers wire format.
It works with ordinary C++ types, requires no generated runtime classes, and does not link against the official
Protobuf runtime.

This document is the complete runtime API guide. Related repository material:

- [source generation](../codegen/README.md);
- [generated-code semantics](../codegen/GENERATED_CODE.md);
- [tutorial](tutorial.md);
- [schema-less decoder example](../examples/decoder/decoder.cpp);
- [file-tree example and benchmark](../examples/filetree/README.md).

Generic `get` and `put` examples use unqualified `PB_*` selectors and assume:

```cpp
using namespace easypb;
```

## 1. Overview, integration, and API model

EasyProtoBuf separates the wire-format engine from the application data model:

```text
C++ object --encode(Encoder&, const T&)--> byte buffer
C++ object <--decode(Decoder, T&)--------- byte buffer
```

The library knows how Protobuf field types are represented on the wire. Application-provided `encode` and
`decode` overloads supply the schema-specific information: field numbers, field types, cardinality, and
presence.

The usual top-level calls are:

```cpp
std::string wire = easypb::encode(source);
Message destination = easypb::decode<Message>(wire);
```

EasyProtoBuf provides:

- scalar, enum, string, bytes, message, repeated, packed, and map field operations;
- generic compile-time operations and equivalent named aliases;
- zero-copy string and bytes reads when requested;
- custom C++ scalar wrappers, string types, ranges, sequence containers, and map-like containers;
- skipping of unknown fields, including unknown legacy groups.

The runtime does not parse or retain a `.proto` schema. C++ message types do not inherit from a library base
class, and choosing field numbers, `PBType` selectors, presence rules, and required-field checks remains the
codec's responsibility.

### 1.1 Header-only integration

Add the repository's `include` directory to the compiler search path and include:

```cpp
#include <easypb.hpp>
```

No EasyProtoBuf binary needs to be linked. For example:

```text
g++ -std=c++11 -Ipath/to/EasyProtoBuf/include application.cpp -o application
```

The minimum language level is C++11. Fixed-width wire values are converted between Protobuf's little-endian
representation and the target CPU's native byte order.

### 1.2 Public API types

| Type | Purpose |
|---|---|
| `easypb::Encoder` | Owns an output buffer and appends encoded fields. |
| `easypb::Decoder` | Non-owning cursor over an existing input buffer. |
| `easypb::PBType` | Compile-time Protobuf type selector used by generic operations. |
| `easypb::string_view` | Configurable non-owning view used for zero-copy reads. |
| `easypb::exception` | Base class for exceptions explicitly defined by the library. |

### 1.3 `easypb::string_view`

The public alias `easypb::string_view` resolves to:

1. `EASYPB_STRING_VIEW`, if that macro is defined before including `easypb.hpp`;
2. `std::string_view`, when the C++17 standard library provides it;
3. EasyProtoBuf's minimal non-owning fallback type otherwise.

An application-supplied view type must provide the operations used by the library, including `data()`, `size()`,
and suitable construction or conversion for the application's string storage. Use the same macro definition in
every translation unit that includes `easypb.hpp`.

For portable owning storage, use `std::string` for both Protobuf `string` and `bytes` fields. Decoder lifetime
rules for non-owning views are described in section 5.4.

### 1.4 Field type table

The alias suffix is shared by named Encoder and Decoder operations. For example, suffix `sint32` forms
`put_sint32`, `put_repeated_sint32`, `put_packed_sint32`, `get_sint32`, and `get_repeated_sint32`.

| `.proto` type / alias suffix | `PBType` | Canonical C++ type | Packable | Map key |
|---|---|---|---|---|
| `int32` | `PB_INT32` | `int32_t` | yes | yes |
| `int64` | `PB_INT64` | `int64_t` | yes | yes |
| `uint32` | `PB_UINT32` | `uint32_t` | yes | yes |
| `uint64` | `PB_UINT64` | `uint64_t` | yes | yes |
| `sint32` | `PB_SINT32` | `int32_t` | yes | yes |
| `sint64` | `PB_SINT64` | `int64_t` | yes | yes |
| `fixed32` | `PB_FIXED32` | `uint32_t` | yes | yes |
| `fixed64` | `PB_FIXED64` | `uint64_t` | yes | yes |
| `sfixed32` | `PB_SFIXED32` | `int32_t` | yes | yes |
| `sfixed64` | `PB_SFIXED64` | `int64_t` | yes | yes |
| `bool` | `PB_BOOL` | `bool` | yes | yes |
| `enum` | `PB_ENUM` | `int32_t` / enum | yes | no |
| `float` | `PB_FLOAT` | `float` | yes | no |
| `double` | `PB_DOUBLE` | `double` | yes | no |
| `string` | `PB_STRING` | string-like | no | yes |
| `bytes` | `PB_BYTES` | string-like | no | no |
| `message` | `PB_MESSAGE` | custom C++ type | no | no |

Generic operations use the selector directly:

```cpp
pb.put<PB_UINT64>(1, value);
pb.put_repeated<PB_STRING>(2, names);
pb.put_packed<PB_SINT32>(3, deltas);
pb.put_map<PB_STRING, PB_INT32>(4, scores);
```

Named map aliases combine the key and value suffixes, such as `put_map_string_int32` and
`get_map_string_int32`. A map key must have `yes` in the last table column; a map value may use any `PBType`,
including `PB_MESSAGE`.

The selected `PBType`, not the source or destination C++ type, determines the Protobuf conversion. A scalar is
converted through its canonical C++ type before encoding or final assignment.

## 2. Complete example

The following example demonstrates the recommended shape of a hand-written codec.

### 2.1 Schema

```proto
syntax = "proto3";

enum State {
  queued = 0;
  running = 1;
  finished = 2;
}

message Attribute {
  string name = 1;
  double value = 2;
}

message Job {
  uint64 id = 1;
  State state = 2;
  repeated sint32 deltas = 3 [packed = true];
  map<string, uint32> counters = 4;
  repeated Attribute attributes = 5;
}
```

### 2.2 C++ data model

```cpp
enum State
{
    STATE_QUEUED = 0,
    STATE_RUNNING = 1,
    STATE_FINISHED = 2,
};

struct Attribute
{
    std::string name;
    double value = 0;
};

struct Job
{
    uint64_t id = 0;
    State state = STATE_QUEUED;
    std::vector<int32_t> deltas;
    std::map<std::string, uint32_t> counters;
    std::vector<Attribute> attributes;
};
```

The structures are ordinary C++ types. They do not inherit from EasyProtoBuf and can contain additional
application-only fields that the codec simply ignores.

The C++ enumerator names need not match the `.proto` identifiers, but their numeric values must.

### 2.3 Encoding overloads

```cpp
inline void encode(easypb::Encoder& pb, const Attribute& value)
{
    pb.put_string(1, value.name);
    pb.put_double(2, value.value);
}

inline void encode(easypb::Encoder& pb, const Job& value)
{
    pb.put_uint64(1, value.id);
    pb.put_enum(2, value.state);
    pb.put_packed_sint32(3, value.deltas);
    pb.put_map_string_uint32(4, value.counters);
    pb.put_repeated_message(5, value.attributes);
}
```

This basic encoder calls every writer unconditionally, including for default or empty singular values and an
empty packed range. Presence policies and conditional encoding are covered separately in section 6.

### 2.4 Decoding overloads

```cpp
inline void decode(easypb::Decoder pb, Attribute& value)
{
    while (pb.get_next_field()) {
        switch (pb.field_num) {
            case 1: pb.get_string(&value.name); break;
            case 2: pb.get_double(&value.value); break;
            default: pb.skip_field();
        }
    }
}

inline void decode(easypb::Decoder pb, Job& value)
{
    while (pb.get_next_field()) {
        switch (pb.field_num) {
            case 1: pb.get_uint64(&value.id); break;
            case 2: pb.get_enum(&value.state); break;
            case 3: pb.get_repeated_sint32(&value.deltas); break;
            case 4: pb.get_map_string_uint32(&value.counters); break;
            case 5: pb.get_repeated_message(&value.attributes); break;
            default: pb.skip_field();
        }
    }
}
```

`get_repeated_sint32` accepts both packed and unpacked occurrences. A map field is represented on the wire
as repeated entry messages, so every outer occurrence calls `get_map_string_uint32` once.

Always consume a recognized field with the matching `get_*` operation, or consume an unrecognized field with
`skip_field()`, before calling `get_next_field()` again.

### 2.5 Encoding and decoding an object

```cpp
Job source;
// ...

const std::string wire = easypb::encode(source);
const Job copy = easypb::decode<Job>(wire);
```

`decode<T>()` value-initializes a new `T`, passes it to `decode(Decoder, T&)`, and returns it by value.
Consequently, `T` must be default-constructible.

The complete compilable program, including object initialization and round-trip validation, is in the
[manual example](../examples/manual/README.md).

## 3. Customization protocol and complete-message helpers

### 3.1 Required overloads

For a message type `T`, define:

```cpp
void encode(easypb::Encoder& pb, const T& value);
void decode(easypb::Decoder pb, T& value);
```

The Encoder is passed by reference because it owns and grows one output buffer. The Decoder is deliberately
passed by value: each copy has an independent cursor over the same non-owned bytes, which is convenient for
nested messages.

Normally, place both overloads in the same namespace as `T` so unqualified calls find them through
argument-dependent lookup (ADL):

```cpp
namespace app
{
struct Message;
void encode(easypb::Encoder&, const Message&);
void decode(easypb::Decoder, Message&);
}
```

For a type from a namespace that cannot be modified, adapters may instead be declared in `easypb`; the Encoder
or Decoder argument makes that namespace participate in lookup. Never add application overloads to `std`.

Definitions in a header should normally be `inline`. Ensure their declarations are visible when message field
operations are instantiated. A type with private state can expose accessors or declare the overloads as friends.

### 3.2 Construction requirements

| Operation | Requirement |
|---|---|
| `put_message` | A matching `encode` overload for the source type. |
| `get_message` | A matching `decode` overload and an existing destination object. |
| `get_repeated_message` | Default construction of the destination container's `value_type`. |
| `get_map<..., PB_MESSAGE>` | Default construction of the map's `mapped_type`. |
| `easypb::decode<T>` | Value initialization of `T`. |

Map decoding finishes with assignment into `operator[]`; move-only mapped values are supported when the map
accepts that final move assignment.

### 3.3 Complete-message helpers

Use the high-level helpers when encoding or decoding one complete message:

```cpp
std::string wire = easypb::encode(value);
Message value = easypb::decode<Message>(wire);
```

`easypb::encode` creates an Encoder, invokes the matching ADL overload, and returns an owning `std::string`.
`easypb::decode<T>` value-initializes `T`, invokes its ADL overload, and returns the result.

The input bytes must remain valid during decoding. If the resulting object stores views returned by
`get_string()` or `get_bytes()`, those bytes must remain valid and unmoved for as long as the views are used.

To decode into an existing object, call its customization overload directly:

```cpp
easypb::Decoder decoder(wire.data(), wire.size());
decode(decoder, existing);
```

Field readers do not clear the destination first. This form therefore merges into existing state; decode into a
fresh object when replacement semantics are required.

## 4. Encoder reference

### 4.1 Lifecycle and output ownership

Create an empty Encoder and append fields:

```cpp
easypb::Encoder pb;
pb.put_uint64(1, id);
pb.put_string(2, name);
std::string wire = pb.result();
```

Each `put*` call appends to an Encoder-owned buffer. `result()` returns the complete logical buffer and resets
the Encoder to an empty state. It is a consuming operation; calling it in the middle of a message separates the
output into different messages. The object can be reused after the call.

### 4.2 Generic and named operation families

The generic compile-time API has four families:

```cpp
pb.put<PB_TYPE>(field_number, value);
pb.put_repeated<PB_TYPE>(field_number, range);
pb.put_packed<PB_TYPE>(field_number, range);
pb.put_map<KEY_PB_TYPE, VALUE_PB_TYPE>(field_number, map_like);
```

Named aliases use the suffixes from the master type table:

```cpp
pb.put_TYPE(field_number, value);
pb.put_repeated_TYPE(field_number, range);
pb.put_packed_TYPE(field_number, range);
pb.put_map_KEYTYPE_VALUETYPE(field_number, map_like);
```

For messages, use `put_message` and `put_repeated_message`; packed messages do not exist. Generic and named
operations have identical semantics, so choose the form that makes the schema easiest to review.

### 4.3 Scalars and enums

A scalar writer converts its input to the canonical C++ type selected by `PBType` before writing it:

```cpp
const int64_t wide = (int64_t{1} << 32) + 3;
pb.put<PB_INT32>(1, wide);  // converts to int32_t first
pb.put_float(2, 1.25);      // converts double to float first
pb.put_bool(3, 17);         // converts to true first
```

This also accepts wrapper types that define an explicit conversion to the canonical type. Normal C++ narrowing
and implementation-defined signed conversion rules apply; perform a checked conversion first when truncation
would be an error.

`sint32` and `sint64` use ZigZag encoding and are generally compact for negative values. They are distinct schema
types, not runtime optimization switches for `int32` and `int64`.

Both scoped and unscoped C++ enums are accepted by `PB_ENUM` and the `*_enum` aliases. Values are converted
through `int32_t`; the runtime does not validate named enumerators or enforce closed-enum semantics.

### 4.4 Strings and bytes

Both Protobuf types are length-delimited:

```cpp
const std::string text = "hello";
const std::string binary("\0\x7f\xff", 3);

pb.put_string(1, text);
pb.put_bytes(2, binary);
```

Input is accepted through `easypb::string_view` and copied into the Encoder buffer during the call. `bytes`
preserves arbitrary values, including embedded zero bytes. `string` receives the same raw treatment; the runtime
does not validate UTF-8.

Do not represent binary data with an API that infers length using a terminating zero. Supply an explicit size:

```cpp
const char raw[] = {'A', '\0', 'B'};
pb.put_bytes(3, easypb::string_view(raw, sizeof(raw)));
```

Encoding a string or bytes value whose length exceeds `INT32_MAX` throws `easypb::length_too_long`.

### 4.5 Repeated and packed fields

`put_repeated` iterates a range and emits one field occurrence per element:

```cpp
pb.put_repeated<PB_SINT32>(1, values);
pb.put_repeated_sint32(1, values);
```

The range only needs range-based `for` support; random access, contiguous storage, and `size()` are not required.
Each element must be accepted by the selected writer. An empty range emits nothing, and iteration order is
preserved.

`put_packed` emits one length-delimited field containing consecutive scalar payloads:

```cpp
pb.put_packed<PB_SINT32>(1, values);
pb.put_packed_sint32(1, values);
```

Packed encoding is available for numeric types, `bool`, and enum, but not for string, bytes, or messages. Invalid
combinations fail during template instantiation.

An empty packed range still emits a tag and a zero-length payload. Guard the call when an empty field should be
absent:

```cpp
if (!values.empty()) {
    pb.put_packed_sint32(1, values);
}
```

The Encoder has no schema and does not infer packedness; the codec chooses the method required by the schema.

### 4.6 Messages

A message is a length-delimited field whose payload is produced by its custom `encode` overload:

```cpp
pb.put<PB_MESSAGE>(1, child);
pb.put_message(2, child);
pb.put_repeated_message(3, children);
```

The source is passed by const reference and is not copied by the field API. An encoded-only message may be
move-only or non-default-constructible; the construction requirements in section 3.2 apply when decoding.

### 4.7 Maps

The generic and named forms are:

```cpp
pb.put_map<PB_STRING, PB_INT32>(1, scores);
pb.put_map_string_int32(1, scores);
```

The key selector must be an integral type, `bool`, or string, as marked in the master table. The value selector
may be any `PBType`, including enum, bytes, or message.

The source may be any iterable map-like object whose entries expose `.first` and `.second`. Each item becomes
one outer field containing a map-entry message with key field `1` and value field `2`. An empty map emits
nothing.

Entries follow container iteration order. Map semantics do not depend on that order, but exact bytes do; use an
ordered container when stable output is required.

### 4.8 Presence, defaults, and field order

The Encoder writes exactly the requested operations. It does not know which fields are optional, required, or
default-valued, and it does not automatically omit zero, false, empty string, or empty message values.

Calling a scalar writer repeatedly emits repeated occurrences. Empty unpacked ranges and maps emit nothing;
empty packed ranges emit one zero-length occurrence. Fields are emitted in call order rather than sorted by
field number. Normal Protobuf readers do not require a particular order, but order affects byte-for-byte output.

## 5. Decoder reference

### 5.1 Non-owning input cursor

Decoder stores pointers into caller-owned input. Construct it from a pointer and size or from
`easypb::string_view`:

```cpp
const std::string wire = obtain_message();

easypb::Decoder by_pointer(wire.data(), wire.size());
const easypb::string_view wire_view(wire);
easypb::Decoder by_view(wire_view);
```

The single-argument `const char*` constructor is deleted; raw buffers require an explicit size.

Do not destroy, move, resize, or otherwise invalidate the input storage while a Decoder or a view returned from
it is in use. Copying a Decoder copies its current position: advancing one copy does not advance another, but
both copies still refer to the same bytes.

### 5.2 Field loop

A schema-aware decoder repeatedly reads a tag, dispatches on its field number, and consumes its payload:

```cpp
void decode(easypb::Decoder pb, Message& value)
{
    while (pb.get_next_field()) {
        switch (pb.field_num) {
            case 1: pb.get_int32(&value.id); break;
            case 2: pb.get_string(&value.name); break;
            default: pb.skip_field();
        }
    }
}
```

`get_next_field()` returns `false` at the end of the current message. Otherwise it reads the next tag, stores the
number in `field_num`, stores the payload category in `wire_type`, positions the cursor at the payload, and
returns `true`.

After every successful call, consume the payload exactly once with a `get*` operation or `skip_field()`.
Advancing without consuming it interprets payload bytes as another tag and loses cursor synchronization.

### 5.3 Scalar operations and presence

Generic scalar readers have returning and pointer forms:

```cpp
int32_t id = pb.get<PB_INT32>();

int64_t widened = 0;
bool has_id = false;
pb.get<PB_INT32>(&widened, &has_id);
```

Named aliases follow the suffixes in the master table:

```cpp
int32_t id = pb.get_int32();
pb.get_int32(&widened, &has_id);
```

The returning form uses the canonical type in the master table. The pointer form first decodes that canonical
type, then constructs and assigns the destination; compatible wrappers and wider destinations are therefore
accepted. Conversion cannot restore information already rounded or truncated by the selected Protobuf type.

After successful assignment, an optional `bool*` presence argument is set to `true`. The Decoder never resets
the flag when a field is absent, so initialize it before entering the loop.

The returning `get_enum()` yields `int32_t`; use `get_enum(&typed_enum)` for an application enum. Messages have
no canonical return type and require `get<PB_MESSAGE>(&object)` or `get_message(&object)`.

### 5.4 String and bytes ownership

The returning readers are zero-copy:

```cpp
easypb::string_view text = pb.get_string();
easypb::string_view data = pb.get_bytes();
```

These views point into the original encoded buffer. They become invalid when that storage is destroyed, moved,
resized, or otherwise relocated.

Pointer readers construct and assign the destination from the decoded view:

```cpp
std::string text;
std::string data;
pb.get_string(&text);
pb.get_bytes(&data);
```

With `std::string`, this copies into owning storage. Custom destination types are supported when they can be
constructed from `easypb::string_view` and assigned; if such a type retains a view, the same input-buffer
lifetime rule still applies.

No UTF-8 validation is performed for `string`; `bytes` returns the same raw payload semantics.

### 5.5 Repeated fields

Generic and named repeated readers append to a caller-supplied container:

```cpp
pb.get_repeated<PB_SINT32>(&values);
pb.get_repeated_sint32(&values);
```

For a packable scalar type, one reader accepts both representations:

- an unpacked occurrence appends one value;
- a packed length-delimited occurrence appends every payload value.

Mixed packed and unpacked occurrences therefore require only one switch case. Strings, bytes, and messages are
always length-delimited per element and are not treated as packed scalar payloads.

The Decoder does not clear the destination. It requires a nested `value_type` and compatible `push_back()`;
repeated messages additionally require default construction of `value_type`:

```cpp
pb.get_repeated<PB_MESSAGE>(&children);
pb.get_repeated_message(&children);
```

### 5.6 Singular messages

Decode a nested message into an existing object:

```cpp
pb.get<PB_MESSAGE>(&child);
pb.get_message(&child, &has_child);
```

The reader invokes the matching `decode` overload without resetting `child`. Multiple occurrences of the same
singular message therefore merge into that object: nested scalar assignments overwrite matching values, while
nested repeated fields append.

### 5.7 Maps

Each map reader call consumes one outer map-entry occurrence:

```cpp
pb.get_map<PB_STRING, PB_INT32>(&scores);
pb.get_map_string_int32(&scores);
```

The outer decoding loop must call it for every occurrence of the map field. The reader value-initializes a key
and mapped value, parses entry fields `1` and `2`, and finally performs assignment equivalent to:

```cpp
scores[std::move(key)] = std::move(value);
```

Map-entry behavior is:

- an omitted key or value keeps its value-initialized default;
- key and value may appear in either order;
- unknown fields inside the entry are skipped;
- repeated scalar key or value fields use the final occurrence;
- repeated message-value fields inside one entry merge into the temporary message;
- a later outer entry with the same key replaces the earlier complete mapped value.

The destination must provide `key_type`, `mapped_type`, value initialization for both, `operator[]`, and
assignment of the final mapped value. Its key and value C++ types must also accept conversions required by the
selected `PBType` values.

### 5.8 Unknown fields and schema-less inspection

Forward-compatible decoders must call `skip_field()` for every unrecognized field. It skips varint, fixed32,
fixed64, length-delimited data, and complete unknown legacy groups while validating their matching field
numbers. Unknown fields are discarded rather than retained for later serialization.

Declared group fields are not supported by the normal field API; group support is limited to skipping unknown
group data already present on the wire.

A schema-less tool may inspect `field_num` and `wire_type` before skipping the payload:

```cpp
while (decoder.get_next_field()) {
    inspect(decoder.field_num, decoder.wire_type);
    decoder.skip_field();
}
```

The wire category cannot distinguish all semantic schema types, so schema-less inspection cannot reconstruct
the original declarations unambiguously.

## 6. Presence, cardinality, and duplicate fields

EasyProtoBuf supplies field operations but does not retain a schema, so the customization overload determines
presence and cardinality policy.

| Field kind | Multiple wire occurrences | Existing destination state |
|---|---|---|
| Singular scalar, enum, string, or bytes | The final decoded assignment wins. | Unchanged when the field is absent. |
| Singular message | Occurrences merge into the same object. | Existing nested state is retained and updated. |
| Repeated field | Every occurrence appends; one packed occurrence may append several values. | Existing elements are retained. |
| Map field | Every occurrence decodes one entry; a later duplicate key replaces the earlier mapped value. | Unmentioned keys are retained. |

### 6.1 Presence flags

Pointer readers for singular fields accept a `bool*` flag:

```cpp
pb.get_string(&value.name, &value.has_name);
pb.get_message(&value.child, &value.has_child);
```

The flag becomes `true` after successful decoding. Initialize it to `false` before the field loop. Repeated and
map readers do not have an equivalent flag; container contents cannot distinguish an absent field from an
explicitly encoded empty packed occurrence.

On encoding, test application presence state before calling a writer when absence must differ from a present
default value:

```cpp
if (value.has_name) {
    pb.put_string(1, value.name);
}
```

### 6.2 Required fields and schema defaults

The runtime defines `easypb::missing_required_field`, but a hand-written decoder must detect required fields
itself:

```cpp
if (!value.has_name) {
    throw easypb::missing_required_field("Message.name is required");
}
```

Run such checks after the field loop. The runtime also does not apply schema-declared defaults; initialize the
C++ object to the desired values before decoding. Absent fields leave those values unchanged, and
`easypb::decode<T>` uses value initialization, including constructors and default member initializers.

## 7. Wire conversions

The Encoder always emits the normal wire form selected by `PBType`. The Decoder deliberately accepts several
compatible payload widths:

| Selected reader | Accepted current wire payload | Conversion rule |
|---|---|---|
| `int*`, `uint*`, fixed/sfixed, `bool`, enum | varint, fixed32, or fixed64 | Interpret as an integer, then convert through the selected canonical type. |
| `sint32`, `sint64` | varint, fixed32, or fixed64 | ZigZag-decode a varint; treat fixed payloads as signed fixed-width integers. |
| `float`, `double` | fixed32 or fixed64 | Read `float` or `double`, then convert to the selected canonical type. |
| string, bytes, message, map | length-delimited only | Interpret the bytes according to the selected semantic type. |
| repeated packable scalar | scalar wire form or length-delimited | Append one unpacked value or every value in the packed payload. |

These conversions permit some schema changes, but sharing a wire category does not guarantee semantic
compatibility. Changing ordinary varint integers to ZigZag changes their interpretation; narrowing, signedness,
`bool` normalization, enum meaning, and floating-point precision can lose information. Arbitrary bytes are not
necessarily a valid embedded message.

Unknown fields are discarded by EasyProtoBuf, so a decode/re-encode cycle does not preserve data introduced by
a newer schema.

## 8. Error handling and input limits

All exception types explicitly defined by EasyProtoBuf derive from `easypb::exception`, which derives from
`std::runtime_error`.

| Exception | Typical cause |
|---|---|
| `easypb::unexpected_eof` | Truncated varint, fixed payload, length-delimited payload, or group. |
| `easypb::varint_too_long` | A varint continues beyond ten bytes. |
| `easypb::length_too_long` | A decoded or encoded string/bytes length exceeds `INT32_MAX`. |
| `easypb::invalid_fieldnum` | A decoded field tag exceeds `UINT32_MAX`. |
| `easypb::wiretype_mismatch` | The selected reader cannot interpret the current wire category. |
| `easypb::unsupported_wiretype` | `skip_field()` encounters an unsupported wire category. |
| `easypb::missing_required_field` | Application codec code detects an absent required field. |

Decode untrusted input around the complete operation:

```cpp
try {
    Message value = easypb::decode<Message>(wire);
    use(value);
} catch (const easypb::exception& error) {
    report_invalid_message(error.what());
}
```

Allocation may separately throw `std::bad_alloc`. Bounds checks prevent reads past the supplied buffer, but a
well-formed message can still contain many repeated/map entries, request large allocations, or create deeply
nested application messages. Enforce application limits on message size, entry counts, and nesting when input is
hostile.

Invalid compile-time combinations, such as packed strings, forbidden map key selectors, value-returning
`get<PB_MESSAGE>()`, missing message customization overloads, or incompatible containers, fail during template
instantiation.

## 9. Observable output properties

Nested messages, map entries, and packed repeated fields reserve a five-byte length prefix. This is a valid
varint representation, but it can be longer than the minimal prefix emitted by another Protobuf encoder.
Interoperable messages therefore need not be byte-for-byte identical.

Exact EasyProtoBuf output also depends on field call order, repeated element order, map iteration order, packed
versus unpacked representation, and whether default or empty values are explicitly emitted. Use stable call and
container order when deterministic bytes are an application requirement; ordinary Protobuf semantic equality
does not define one unique encoding.
