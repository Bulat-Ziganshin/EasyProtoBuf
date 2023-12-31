Generate code from .proto files in two steps:
1. Use official Protoc compiler to generate binary file: `protoc.exe tutorial.proto -otutorial.pbs`
2. Compile it with Codegen: `generator.exe tutorial.pbs >tutorial.pb.cpp`

Files:
- [descriptor.pb.cpp](descriptor.pb.cpp) - C++ structures and ProtoBuf decoders manually generated from
[descriptor.proto](https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto)
- [codegen.cpp](codegen.cpp) - the program
