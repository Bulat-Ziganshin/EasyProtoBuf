Files:
- [tutorial.proto](tutorial.proto) - ProtoBuf definition of the serialized structure
- [tutorial.pb.cpp](tutorial.pb.cpp) - auto-generated corresponding C++ structure and ProtoBuf encoder/decoder for it
- [main.cpp](main.cpp) - sample client code that encodes message, decodes it back and checks equality of the original and the decoded message

## Demonstrated Codegen features

- generating plain C++ structures and their ProtoBuf encoders/decoders
- proto2 required and optional fields, default values, `has_*` flags,
and required-field checks
- scalar, string/bytes, and message fields
- repeated scalar, string, and message fields
- a map with scalar key and value types
- the `EASYPB_*_EXTRA_FIELDS`, `EASYPB_*_EXTRA_ENCODING`, and
`EASYPB_*_EXTRA_DECODING` insertion points, used here to manually implement
a proto2 extension not handled by Codegen itself
