// This file will be auto-generated when I grow up (with option "-s str_view").
// Source: https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto
#ifndef EASYPB_DESCRIPTOR_PB_CPP_INCLUDED
#define EASYPB_DESCRIPTOR_PB_CPP_INCLUDED

#include <cstdint>
#include <string>
#include <vector>
#if __cplusplus >= 201703L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
#include <string_view>
#endif

#ifdef __cpp_lib_string_view
using str_view = std::string_view;  // Might be a little faster with C++17
#else
using str_view = std::string;
#endif

#include <easypb.hpp>


struct OneofDescriptorProto
{
    str_view name;

    bool has_name = false;
};


// Single enum value
struct EnumValueDescriptorProto
{
    str_view name;
    int32_t number = 0;

    bool has_name = false;
    bool has_number = false;
};


// Enum
struct EnumDescriptorProto
{
    str_view name;
    std::vector<EnumValueDescriptorProto> value;

    bool has_name = false;
};


struct FieldOptions
{
    bool packed = false;

    bool has_packed = false;
};


// Field
struct FieldDescriptorProto
{
    enum {
        TYPE_DOUBLE = 1,
        TYPE_FLOAT = 2,
        TYPE_INT64 = 3,
        TYPE_UINT64 = 4,
        TYPE_INT32 = 5,
        TYPE_FIXED64 = 6,
        TYPE_FIXED32 = 7,
        TYPE_BOOL = 8,
        TYPE_STRING = 9,
        TYPE_GROUP = 10,
        TYPE_MESSAGE = 11,
        TYPE_BYTES = 12,
        TYPE_UINT32 = 13,
        TYPE_ENUM = 14,
        TYPE_SFIXED32 = 15,
        TYPE_SFIXED64 = 16,
        TYPE_SINT32 = 17,
        TYPE_SINT64 = 18,
    };

    enum {
      LABEL_OPTIONAL = 1,
      LABEL_REPEATED = 3,
      LABEL_REQUIRED = 2,
    };

    str_view name;
    int32_t number = 0;
    int32_t label = 0;
    int32_t type = 0;
    str_view type_name;
    str_view default_value;
    FieldOptions options;
    int32_t oneof_index = 0;

    bool has_name = false;
    bool has_number = false;
    bool has_label = false;
    bool has_type = false;
    bool has_type_name = false;
    bool has_default_value = false;
    bool has_options = false;
    bool has_oneof_index = false;
};


struct MessageOptions
{
    bool map_entry = false;

    bool has_map_entry = false;
};


// Message
struct DescriptorProto
{
    str_view name;
    std::vector<FieldDescriptorProto> field;
    std::vector<DescriptorProto> nested_type;
    std::vector<EnumDescriptorProto> enum_type;
    std::vector<OneofDescriptorProto> oneof_decl;
    MessageOptions options;

    bool has_name = false;
    bool has_options = false;
};


// Single .proto file
struct FileDescriptorProto
{
    str_view name;
    str_view package;
    std::vector<DescriptorProto> message_type;
    std::vector<EnumDescriptorProto> enum_type;
    str_view syntax;

    bool has_name = false;
    bool has_package = false;
    bool has_syntax = false;
};


// Multiple .proto files
struct FileDescriptorSet
{
    std::vector<FileDescriptorProto> file;
};


inline void decode(easypb::Decoder pb, OneofDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(easypb::Decoder pb, EnumValueDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            case 2: pb.get_int32(&x.number, &x.has_number); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(easypb::Decoder pb, EnumDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            case 2: pb.get_repeated_message(&x.value); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(easypb::Decoder pb, FieldOptions &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 2: pb.get_bool(&x.packed, &x.has_packed); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(easypb::Decoder pb, FieldDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name,          &x.has_name); break;
            case 3: pb.get_int32 (&x.number,        &x.has_number); break;
            case 4: pb.get_enum  (&x.label,         &x.has_label); break;
            case 5: pb.get_enum  (&x.type,          &x.has_type); break;
            case 6: pb.get_string(&x.type_name,     &x.has_type_name); break;
            case 7: pb.get_string(&x.default_value, &x.has_default_value); break;
            case 8: pb.get_message(&x.options,      &x.has_options); break;
            case 9: pb.get_int32 (&x.oneof_index,   &x.has_oneof_index); break;
            default: pb.skip_field();
        }
    }

    if(! x.has_name) {
        throw easypb::missing_required_field("Decoded protobuf has no required field FieldDescriptorProto.name");
    }
}


inline void decode(easypb::Decoder pb, MessageOptions &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 7: pb.get_bool(&x.map_entry, &x.has_map_entry); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(easypb::Decoder pb, DescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            case 2: pb.get_repeated_message(&x.field); break;
            case 3: pb.get_repeated_message(&x.nested_type); break;
            case 4: pb.get_repeated_message(&x.enum_type); break;
            case 8: pb.get_repeated_message(&x.oneof_decl); break;
            case 7: pb.get_message(&x.options, &x.has_options); break;
            default: pb.skip_field();
        }
    }

    if(! x.has_name) {
        throw easypb::missing_required_field("Decoded protobuf has no required field DescriptorProto.name");
    }
}


inline void decode(easypb::Decoder pb, FileDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            case 2: pb.get_string(&x.package, &x.has_package); break;
            case 4: pb.get_repeated_message(&x.message_type); break;
            case 5: pb.get_repeated_message(&x.enum_type); break;
            case 12: pb.get_string(&x.syntax, &x.has_syntax); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(easypb::Decoder pb, FileDescriptorSet &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_repeated_message(&x.file); break;
            default: pb.skip_field();
        }
    }
}

#endif
