Generate code from .proto files in two steps:
1. Use the official [protoc](https://github.com/protocolbuffers/protobuf/releases) compiler to generate binary file: `protoc tutorial.proto -otutorial.pbs`
2. Compile it with Codegen: `codegen tutorial.pbs >tutorial.pb.cpp`

Files:
- [main.cpp](main.cpp) - main(), including cmdline option parser and file I/O
- [codegen.cpp](codegen.cpp) - generator() which translates FileDescriptorProto into C++ code
- [descriptor.pb.cpp](descriptor.pb.cpp) - C++ structures and ProtoBuf decoders manually generated from
[descriptor.proto](https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto)
- [utils.cpp](utils.cpp) - common utility functions


## Options

`-s, --string-type arg (=std::string)    C++ type for string/bytes fields`

C++ type used for all string and bytes fields.
You can use e.g. std::string_view when messages are only decoded,
and never outlive the decoded buffer.

`-r, --repeated-type arg (=std::vector)  C++ container type for repeated fields`

Use for all repeated fields another container (e.g. std::deque).
"{}" in the name replaced by the C++ field type,
e.g. "vector<{}>" for "repeated int32" type will be formatted as "vector<int32_t>".

If "{}" is absent in the argument, then "<{}>" will be automatically added to it.

Codegen automatically includes `<vector>`. When using another container,
ensure that its header is included before the generated code.

`-m, --map-type arg (=std::map)          C++ container type for map fields`

Use another container for all map fields. "{0}" and "{1}" are replaced
by the C++ key and value types respectively. For example,
"custom_map<{0},{1}>" applied to "map<string, int32>" produces
"custom_map<std::string,int32_t>".

If no placeholders are present in the argument,
then "<{0},{1}>" will be automatically added to it.

Codegen automatically includes `<map>`. When using another container,
ensure that its header is included before the generated code.

Codegen supports scalar and enum map values. Enum values are represented
as int32_t. Message-valued maps are not supported yet.


## Details

There are 4 code insertion points for each message type:
- EASYPB_{0}_EXTRA_FIELDS
- EASYPB_{0}_EXTRA_ENCODING
- EASYPB_{0}_EXTRA_DECODING
- EASYPB_{0}_EXTRA_POST_DECODING

... where `{0}` is replaced by the class name. They allow the addition of extra fields
and their (de)serialization code, thus mixing auto-generated and manually-written code.
Check their usage in the [Tutorial](../examples/tutorial).
