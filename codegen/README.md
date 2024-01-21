Generate code from .proto files in two steps:
1. Use the official [protoc](https://github.com/protocolbuffers/protobuf/releases) compiler to generate binary file: `protoc tutorial.proto -otutorial.pbs`
2. Compile it with Codegen: `codegen tutorial.pbs >tutorial.pb.cpp`

Files:
- [main.cpp](main.cpp) - main(), including cmdline option parser and file I/O
- [codegen.cpp](codegen.cpp) - generator() which translates FileDescriptorProto into C++ code
- [descriptor.pb.cpp](descriptor.pb.cpp) - C++ structures and ProtoBuf decoders manually generated from
[descriptor.proto](https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto)
- [utils.cpp](utils.cpp) - common utility functions


## Details

There are 4 code insertion points for each message type:
- EASYPB_{0}_EXTRA_FIELDS
- EASYPB_{0}_EXTRA_ENCODING
- EASYPB_{0}_EXTRA_DECODING
- EASYPB_{0}_EXTRA_POST_DECODING

... where `{0}` is replaced by the class name. They allow the addition of extra fields
and their (de)serialization code, thus mixing auto-generated and manually-written code.
Check their usage in the [Tutorial](../examples/tutorial).
