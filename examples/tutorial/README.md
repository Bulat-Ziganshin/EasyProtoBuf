# Tutorial

Files:
- [tutorial.proto](tutorial.proto) — Protobuf definition of the serialized structures
- [tutorial.pb.hpp](tutorial.pb.hpp) — generated C++ header for the schema
- [main.cpp](main.cpp) — sample client code that encodes a message, decodes it back, and checks the result

## Demonstrated Codegen features

- generated C++ structures and their ProtoBuf encoders/decoders
- proto2 required and optional fields, default values, `has_*` flags, and required-field checks
- scalar, string/bytes, and message fields
- repeated scalar, string, and message fields
- a map with scalar key and value types
- the `EASYPB_*_EXTRA_FIELDS`, `EASYPB_*_EXTRA_ENCODING`, and
  `EASYPB_*_EXTRA_DECODING` insertion points, used here to implement a proto2 extension
  not handled by Codegen itself
