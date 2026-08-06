# EasyProtoBuf Codegen

Generate C++ code from a `.proto` schema in two steps:

1. Use the official [`protoc`](https://github.com/protocolbuffers/protobuf/releases) compiler to create a binary descriptor set:
   `protoc tutorial.proto -otutorial.pbs`
2. Run EasyProtoBuf Codegen:
   `codegen tutorial.pbs >tutorial.pb.cpp`

The generated file contains plain C++ structures followed by free codec overloads:

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
- [descriptor.pb.cpp](descriptor.pb.cpp) — C++ structures and ProtoBuf decoders for
  [`descriptor.proto`](https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto)
- [utils.cpp](utils.cpp) — common utility functions

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
Include the corresponding container header before the generated file.

`-m, --map-type arg (=std::map)` selects the container for map fields.
`{0}` and `{1}` are replaced by the key and value types. If no placeholders are present,
`<{0},{1}>` is appended. Include the corresponding container header before the generated file.

Codegen currently supports scalar and enum map values. Enum values are represented as `int32_t`.
Message-valued maps are not supported yet.

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

Define insertion macros before including the generated file. See the [Tutorial](../examples/tutorial).
