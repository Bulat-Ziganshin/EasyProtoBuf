// This file will be auto-generated when I grow up.
// Source: https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto
#include <cstdint>
#include <string>
#include <vector>


struct OneofDescriptorProto
{
    std::string_view name;

    bool has_name = false;

    void decode(easypb::Decoder pb);
};


// Single enum value
struct EnumValueDescriptorProto
{
    std::string_view name;
    int32_t number = 0;

    bool has_name = false;
    bool has_number = false;

    void decode(easypb::Decoder pb);
};


// Enum
struct EnumDescriptorProto
{
    std::string_view name;
    std::vector<EnumValueDescriptorProto> value;

    bool has_name = false;

    void decode(easypb::Decoder pb);
};


struct FieldOptions
{
    bool packed = false;

    bool has_packed = false;

    void decode(easypb::Decoder pb);
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

    std::string_view name;
    int32_t number = 0;
    int32_t label = 0;
    int32_t type = 0;
    std::string_view type_name;
    std::string_view default_value;
    FieldOptions options;

    bool has_name = false;
    bool has_number = false;
    bool has_label = false;
    bool has_type = false;
    bool has_type_name = false;
    bool has_default_value = false;
    bool has_options = false;

    void decode(easypb::Decoder pb);
};


struct MessageOptions
{
    bool map_entry = false;

    bool has_map_entry = false;

    void decode(easypb::Decoder pb);
};


// Message
struct DescriptorProto
{
    std::string_view name;
    std::vector<FieldDescriptorProto> field;
    std::vector<DescriptorProto> nested_type;
    std::vector<EnumDescriptorProto> enum_type;
    std::vector<OneofDescriptorProto> oneof_decl;
    MessageOptions options;

    bool has_name = false;
    bool has_options = false;

    void decode(easypb::Decoder pb);
};


// Single .proto file
struct FileDescriptorProto
{
    std::string_view name;
    std::string_view package;
    std::vector<DescriptorProto> message_type;
    std::vector<EnumDescriptorProto> enum_type;

    bool has_name = false;
    bool has_package = false;

    void decode(easypb::Decoder pb);
};


// Multiple .proto files
struct FileDescriptorSet
{
    std::vector<FileDescriptorProto> file;

    void decode(easypb::Decoder pb);
};


void OneofDescriptorProto::decode(easypb::Decoder pb)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&name, &has_name); break;
            default: pb.skip_field();
        }
    }
}


void EnumValueDescriptorProto::decode(easypb::Decoder pb)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&name, &has_name); break;
            case 2: pb.get_int32(&number, &has_number); break;
            default: pb.skip_field();
        }
    }
}


void EnumDescriptorProto::decode(easypb::Decoder pb)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&name, &has_name); break;
            case 2: pb.get_repeated_message(&value); break;
            default: pb.skip_field();
        }
    }
}


void FieldOptions::decode(easypb::Decoder pb)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 2: pb.get_bool(&packed, &has_packed); break;
            default: pb.skip_field();
        }
    }
}


void FieldDescriptorProto::decode(easypb::Decoder pb)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&name,          &has_name); break;
            case 3: pb.get_int32 (&number,        &has_number); break;
            case 4: pb.get_enum  (&label,         &has_label); break;
            case 5: pb.get_enum  (&type,          &has_type); break;
            case 6: pb.get_string(&type_name,     &has_type_name); break;
            case 7: pb.get_string(&default_value, &has_default_value); break;
            case 8: pb.get_message(&options,      &has_options); break;
            default: pb.skip_field();
        }
    }

    if(! has_name) {
        throw std::runtime_error("Decoded protobuf has no required field FieldDescriptorProto.name");
    }
}


void MessageOptions::decode(easypb::Decoder pb)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 7: pb.get_bool(&map_entry, &has_map_entry); break;
            default: pb.skip_field();
        }
    }
}


void DescriptorProto::decode(easypb::Decoder pb)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&name, &has_name); break;
            case 2: pb.get_repeated_message(&field); break;
            case 3: pb.get_repeated_message(&nested_type); break;
            case 4: pb.get_repeated_message(&enum_type); break;
            case 8: pb.get_repeated_message(&oneof_decl); break;
            case 7: pb.get_message(&options, &has_options); break;
            default: pb.skip_field();
        }
    }

    if(! has_name) {
        throw std::runtime_error("Decoded protobuf has no required field DescriptorProto.name");
    }
}


void FileDescriptorProto::decode(easypb::Decoder pb)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&name, &has_name); break;
            case 2: pb.get_string(&package, &has_package); break;
            case 4: pb.get_repeated_message(&message_type); break;
            case 5: pb.get_repeated_message(&enum_type); break;
            default: pb.skip_field();
        }
    }
}


void FileDescriptorSet::decode(easypb::Decoder pb)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_repeated_message(&file); break;
            default: pb.skip_field();
        }
    }
}
