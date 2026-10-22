// This file will be auto-generated when I grow up (with option "-s str_view").
// Source: https://github.com/protocolbuffers/protobuf/blob/main/src/google/protobuf/descriptor.proto
#ifndef EASYPB_DESCRIPTOR_PB_HPP_INCLUDED
#define EASYPB_DESCRIPTOR_PB_HPP_INCLUDED

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


// Enum options. Only `allow_alias` is modeled: with it set, two values of one
// enum may share a number, and without it that shape is a defect the linker
// refuses (D-28). It is the second field of EnumOptions in descriptor.proto.
struct EnumOptions
{
    bool allow_alias = false;

    bool has_allow_alias = false;
};


// Enum
struct EnumDescriptorProto
{
    str_view name;
    std::vector<EnumValueDescriptorProto> value;
    EnumOptions options;

    bool has_name = false;
    bool has_options = false;
};


// EasyProtoBuf-specific field options. The extension number is mirrored by
// codegen/easypb/options.proto so .proto parsing and .pbs decoding normalize
// into the same trimmed descriptor representation.
enum { EASYPB_CPP_FIELD_OPTIONS_NUMBER = 52777 };

struct CppFieldOptions
{
    str_view type;

    bool has_type = false;
};


struct FieldOptions
{
    bool packed = false;
    CppFieldOptions cpp;

    bool has_packed = false;
    bool has_cpp = false;
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


// One `extensions START to END;` declaration. `start` is inclusive and `end`
// exclusive, exactly as descriptor.proto defines the pair
// (`DescriptorProto.ExtensionRange.start`/`.end`). The plan-5 follow-up's
// session S5 (`W-45`) models the pair instead of counting it: protoc refuses a
// proto3 extension range and a range that includes a declared field number
// (`descriptor.cc:6426-6430`, `:7898`), and neither question can be answered
// from a count.
struct DescriptorProto_ExtensionRange
{
    int32_t start = 0;  // field 1, inclusive
    int32_t end = 0;    // field 2, exclusive

    bool has_start = false;
    bool has_end = false;
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
    // Wire field 5: the extension ranges themselves, decoded since `W-45`
    // (the proto3 and field-overlap rules need their numbers).
    std::vector<DescriptorProto_ExtensionRange> extension_range;
    // Wire field 6 (`extension`). Still counted, not decoded: no component
    // reads extension members, and the only question asked about them is
    // whether a synthetic map entry declares any (protoc's `ValidateMapEntry`,
    // `descriptor.cc:8448-8452`, ledger `D-62`). A count cannot throw on a
    // required member of a shape nothing else reads.
    int32_t extension_count = 0;        // field 6, occurrence count

    bool has_name = false;
    bool has_options = false;
};


// Single .proto method. The NAME is retained so the lexical name sweep can
// check it (ledger `D-57`), and the input/output type names are retained so the
// linker can resolve them (`W-16`): protoc's `CrossLinkMethod` refuses a name
// that resolves nowhere and one that resolves to something other than a
// message (`descriptor.cc:7714-7757`), and the source frontend used to discard
// the whole method table, so neither shape could ever be refused there.
struct MethodDescriptorProto
{
    str_view name;
    str_view input_type;   // field 2
    str_view output_type;  // field 3

    bool has_name = false;
    bool has_input_type = false;
    bool has_output_type = false;
};


// Single .proto service. The name is retained so it participates in
// package-scope collision validation; the method names are retained so the
// lexical name sweep can cover them, which is what closes `D-57`.
struct ServiceDescriptorProto
{
    str_view name;
    std::vector<MethodDescriptorProto> method;

    bool has_name = false;
};


// Single .proto file
struct FileDescriptorProto
{
    str_view name;
    str_view package;
    std::vector<DescriptorProto> message_type;
    std::vector<EnumDescriptorProto> enum_type;
    std::vector<ServiceDescriptorProto> service;
    str_view syntax;
    // Field 14: the edition a file declares (`edition = "2023";`). protoc 30.1
    // writes it for editions files only, beside `syntax = "editions"`; the
    // classifier consults it FIRST, mirroring `BuildFileImpl`
    // (descriptor.cc:5952-5971), so a crafted set that carries the field with
    // no syntax string is classified like the reference and not downgraded to
    // proto2 (ledger `D-52`).
    int32_t edition = 0;
    std::vector<str_view> dependency;
    std::vector<int32_t> public_dependency;
    std::vector<int32_t> weak_dependency;

    bool has_name = false;
    bool has_package = false;
    bool has_syntax = false;
    bool has_edition = false;
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


inline void decode(easypb::Decoder pb, EnumOptions &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 2: pb.get_bool(&x.allow_alias, &x.has_allow_alias); break;
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
            case 3: pb.get_message(&x.options, &x.has_options); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(easypb::Decoder pb, CppFieldOptions &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.type, &x.has_type); break;
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
            case EASYPB_CPP_FIELD_OPTIONS_NUMBER:
                pb.get_message(&x.cpp, &x.has_cpp); break;
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


inline void decode(easypb::Decoder pb, DescriptorProto_ExtensionRange &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_int32(&x.start, &x.has_start); break;
            case 2: pb.get_int32(&x.end, &x.has_end); break;
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
            case 5: pb.get_repeated_message(&x.extension_range); break;
            // Field 6 is counted, not decoded (see the struct): the skip is
            // the whole read, and the count is what A26's sweep asks
            // (ledger `D-62`).
            case 6: ++x.extension_count; pb.skip_field(); break;
            case 8: pb.get_repeated_message(&x.oneof_decl); break;
            case 7: pb.get_message(&x.options, &x.has_options); break;
            default: pb.skip_field();
        }
    }

    if(! x.has_name) {
        throw easypb::missing_required_field("Decoded protobuf has no required field DescriptorProto.name");
    }
}


inline void decode(easypb::Decoder pb, MethodDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            case 2: pb.get_string(&x.input_type, &x.has_input_type); break;
            case 3: pb.get_string(&x.output_type, &x.has_output_type); break;
            default: pb.skip_field();
        }
    }
}


inline void decode(easypb::Decoder pb, ServiceDescriptorProto &x)
{
    while(pb.get_next_field())
    {
        switch(pb.field_num)
        {
            case 1: pb.get_string(&x.name, &x.has_name); break;
            case 2: pb.get_repeated_message(&x.method); break;
            default: pb.skip_field();
        }
    }

    if(! x.has_name) {
        throw easypb::missing_required_field("Decoded protobuf has no required field ServiceDescriptorProto.name");
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
            case 3: pb.get_repeated_string(&x.dependency); break;
            case 4: pb.get_repeated_message(&x.message_type); break;
            case 5: pb.get_repeated_message(&x.enum_type); break;
            case 6: pb.get_repeated_message(&x.service); break;
            case 10: pb.get_repeated_int32(&x.public_dependency); break;
            case 11: pb.get_repeated_int32(&x.weak_dependency); break;
            case 12: pb.get_string(&x.syntax, &x.has_syntax); break;
            case 14: pb.get_enum  (&x.edition, &x.has_edition); break;
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
