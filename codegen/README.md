Generate code from .proto files in two steps:
1. Use the official [protoc](https://github.com/protocolbuffers/protobuf/releases) compiler to generate binary file: `protoc.exe tutorial.proto -otutorial.pbs`
2. Compile it with Codegen: `generator.exe tutorial.pbs >tutorial.pb.cpp`

Files:
- [main.cpp](main.cpp) - main(), including cmdline option parser and file I/O
- [codegen.cpp](codegen.cpp) - generator() which translates FileDescriptorProto into C++ code
- [descriptor.pb.cpp](descriptor.pb.cpp) - C++ structures and ProtoBuf decoders manually generated from
[descriptor.proto](https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto)
- [utils.cpp](utils.cpp) - common utility functions
