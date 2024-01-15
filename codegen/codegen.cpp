#include <string>
#include <cctype>
#include <iostream>
#include <fstream>
#include <iterator>
#include <format>

#include <easypb/decoder.hpp>
#include "descriptor.pb.cpp"
#include "utils.cpp"


// Protobuf and C++ delimiters between parts of a qualified name
const std::string PB_TYPE_DELIMITER = ".";
const std::string CPP_TYPE_DELIMITER = "::";


// Command-line options affecting the generated code
struct
{
    bool no_class;
    bool no_decoder;
    bool no_encoder;
    bool no_has_fields;
    bool no_required;
    bool no_default_values;
    bool packed;
    bool no_packed;
    std::string cpp_string_type;
    std::string cpp_repeated_type;
} option;


auto FILE_TEMPLATE = make_format(
R"---(// Generated by a ProtoBuf compiler.  DO NOT EDIT!
// Source: {0}

#include <cstdint>
#include <string>
#include <vector>

)---");


// {0}=message_type.name, {1}=fields_defs, {2}=has_fields_defs, {3}=encoder declaration, {4}=decoder declaration
auto CLASS_TEMPLATE = make_format(R"---(
struct {0}
{{
{1}
{2}
{3}
{4}
}};
)---");

const char* ENCODER_DECLARATION_TEMPLATE =
"    void encode(easypb::Encoder &pb);";

const char* DECODER_DECLARATION_TEMPLATE =
"    void decode(easypb::Decoder pb);";


// {0}=message_type.name, {1}=encoder
auto ENCODER_TEMPLATE = make_format(R"---(
void {0}::encode(easypb::Encoder &pb)
{{
{1}
}}
)---");


// {0}=message_type.name, {1}=decode_cases, {2}=check_required_fields
auto DECODER_TEMPLATE = make_format(R"---(
void {0}::decode(easypb::Decoder pb)
{{
    while(pb.get_next_field())
    {{
        switch(pb.field_num)
        {{
{1}
            default: pb.skip_field();
        }}
    }}
{2}
}}
)---");


// {0}=message_type.name, {1}=field.name
auto CHECK_REQUIRED_FIELD_TEMPLATE = make_format(R"---(
    if(! has_{1}) {{
        throw std::runtime_error("Decoded protobuf has no required field {0}.{1}");
    }}
)---");



std::string_view protobuf_type_as_str(FieldDescriptorProto &field)
{
    switch(field.type)
    {
        case FieldDescriptorProto::TYPE_DOUBLE:    return "double";
        case FieldDescriptorProto::TYPE_FLOAT:     return "float";
        case FieldDescriptorProto::TYPE_INT64:     return "int64";
        case FieldDescriptorProto::TYPE_UINT64:    return "uint64";
        case FieldDescriptorProto::TYPE_INT32:     return "int32";
        case FieldDescriptorProto::TYPE_FIXED64:   return "fixed64";
        case FieldDescriptorProto::TYPE_FIXED32:   return "fixed32";
        case FieldDescriptorProto::TYPE_BOOL:      return "bool";
        case FieldDescriptorProto::TYPE_STRING:    return "string";
        case FieldDescriptorProto::TYPE_GROUP:     return "?group";
        case FieldDescriptorProto::TYPE_MESSAGE:   return "message";
        case FieldDescriptorProto::TYPE_BYTES:     return "bytes";
        case FieldDescriptorProto::TYPE_UINT32:    return "uint32";
        case FieldDescriptorProto::TYPE_ENUM:      return "enum";
        case FieldDescriptorProto::TYPE_SFIXED32:  return "sfixed32";
        case FieldDescriptorProto::TYPE_SFIXED64:  return "sfixed64";
        case FieldDescriptorProto::TYPE_SINT32:    return "sint32";
        case FieldDescriptorProto::TYPE_SINT64:    return "sint64";
    }

    return "?type";
}


// Return the shortest [qualified] C++ type corresponding to the fully qualified Protobuf message/enum type
std::string cpp_qualified_type_str(std::string_view package_name_prefix, std::string_view msgtype_name_prefix, std::string_view message_type)
{
    // Strip the package_name_prefix from the fully qualified message_type,
    // e.g. (".google.protobuf.", ".google.protobuf.DescriptorProto.ExtensionRange") -> "DescriptorProto.ExtensionRange",
    // then drop the prefix of the current msgtype (e.g. "DescriptorProto."),
    // and finally replace "." typename separator with "::"

    // If message type starts with ".PACKAGE_NAME.", drop this prefix
    auto prefix_len = package_name_prefix.length();
    if (message_type.substr(0, prefix_len) == package_name_prefix) {
        message_type = message_type.substr(prefix_len);

        // If message type starts with "CURRENT_MSGTYPE_NAME.", drop this prefix
        auto prefix_len = msgtype_name_prefix.length();
        if (message_type.substr(0, prefix_len) == msgtype_name_prefix) {
            message_type = message_type.substr(prefix_len);
        }
    }

    // Finally, replace "." between name components with C++-specific "::"
    return string_replace_all(std::string(message_type), PB_TYPE_DELIMITER, CPP_TYPE_DELIMITER );
}


// Return C++ type corresponding to the base type (without "repeated") of the Protobuf field
std::string base_cpp_type_as_str(std::string_view package_name_prefix, std::string_view msgtype_name_prefix, FieldDescriptorProto &field)
{
    // According to https://github.com/protocolbuffers/protobuf/blob/c05b320d9c18173bfce36c4bef22f9953d340ff9/src/google/protobuf/descriptor.h#L780
    switch(field.type)
    {
        case FieldDescriptorProto::TYPE_INT32:
        case FieldDescriptorProto::TYPE_SINT32:
        case FieldDescriptorProto::TYPE_SFIXED32: return "int32_t";

        case FieldDescriptorProto::TYPE_INT64:
        case FieldDescriptorProto::TYPE_SINT64:
        case FieldDescriptorProto::TYPE_SFIXED64: return "int64_t";

        case FieldDescriptorProto::TYPE_UINT32:
        case FieldDescriptorProto::TYPE_FIXED32:  return "uint32_t";

        case FieldDescriptorProto::TYPE_UINT64:
        case FieldDescriptorProto::TYPE_FIXED64:  return "uint64_t";

        case FieldDescriptorProto::TYPE_DOUBLE:   return "double";

        case FieldDescriptorProto::TYPE_FLOAT:    return "float";

        case FieldDescriptorProto::TYPE_BOOL:     return "bool";

        case FieldDescriptorProto::TYPE_ENUM:     return "int32_t";

        case FieldDescriptorProto::TYPE_STRING:
        case FieldDescriptorProto::TYPE_BYTES:    return option.cpp_string_type;

        case FieldDescriptorProto::TYPE_MESSAGE:  return cpp_qualified_type_str(package_name_prefix, msgtype_name_prefix, field.type_name);

        case FieldDescriptorProto::TYPE_GROUP:    return "?group";
    }

    return "?type";
}


// Return C++ type for the Protobuf field
std::string cpp_type_as_str(std::string_view package_name_prefix, std::string_view msgtype_name_prefix, FieldDescriptorProto &field)
{
    auto result = base_cpp_type_as_str(package_name_prefix, msgtype_name_prefix, field);

    if (field.label == FieldDescriptorProto::LABEL_REPEATED) {
        return option.cpp_repeated_type + std::format("<{}>", result);
    } else {
        return result;
    }
}


// Does this field type supports serialisation to the more compact "packed" Protobuf wire format?
bool can_be_packed(FieldDescriptorProto &field)
{
    return (field.label == FieldDescriptorProto::LABEL_REPEATED) &&
           (field.type != FieldDescriptorProto::TYPE_STRING) &&
           (field.type != FieldDescriptorProto::TYPE_BYTES) &&
           (field.type != FieldDescriptorProto::TYPE_MESSAGE) &&
           (field.type != FieldDescriptorProto::TYPE_GROUP);
}


void generator(FileDescriptorSet &proto)
{
    auto file = proto.file[0];
    auto package_name_prefix =
        file.package > ""
            ? PB_TYPE_DELIMITER + std::string(file.package) + PB_TYPE_DELIMITER
            : PB_TYPE_DELIMITER;

    for (auto message_type: file.message_type)
    {
        std::string fields_defs, has_fields_defs, encoder, decode_cases, check_required_fields;
        auto msgtype_name_prefix = std::string(message_type.name) + PB_TYPE_DELIMITER;

        for (auto field: message_type.field)
        {
            auto pbtype_str = protobuf_type_as_str(field);  // PB type as used in .proto file (e.g. "fixed32")
            auto cpptype_str = cpp_type_as_str(package_name_prefix, msgtype_name_prefix, field);  // C++ type for the field (e.g. "std::vector<int32_t>")

            // Generate message structure
            std::string default_str;
            if (field.has_default_value  &&  ! option.no_default_values) {
                bool is_bytearray_field = (field.type==FieldDescriptorProto::TYPE_STRING || field.type==FieldDescriptorProto::TYPE_BYTES);
                const char* quote_str = (is_bytearray_field? "\"" : "");
                default_str = std::format(" = {0}{1}{0}", quote_str, field.default_value);
            }

            fields_defs += std::format("    {} {}{};\n", cpptype_str, field.name, default_str);

            if ((field.label != FieldDescriptorProto::LABEL_REPEATED)  &&  ! option.no_has_fields) {
                has_fields_defs += std::format("    bool has_{} = false;\n", field.name);
            }


            // Generate message encoding function
            bool packed_field = (field.has_options && field.options.has_packed && field.options.packed);
            bool write_as_packed = option.packed ?    true :
                                   option.no_packed ? false :
                                                      packed_field;
            auto type_prefix = (field.label == FieldDescriptorProto::LABEL_REPEATED
                                    ? (write_as_packed && can_be_packed(field)? "packed_":"repeated_")
                                    : "");
            encoder += std::format("    pb.put_{0}{1}({2}, {3});\n", type_prefix, pbtype_str, field.number, field.name);


            // Generate message decoding function
            std::string get_call;

            if (field.label == FieldDescriptorProto::LABEL_REPEATED) {
                get_call = std::format("pb.get_repeated_{0}(&{1})", pbtype_str, field.name);
            } else {
                std::string extra_parameter = "";
                if(! option.no_has_fields) {
                    extra_parameter = std::format(", &has_{0}", field.name);
                }
                get_call = std::format("pb.get_{0}(&{1}{2})", pbtype_str, field.name, extra_parameter);
            }

            decode_cases += std::format("            case {}: {}; break;\n", field.number, get_call);

            if ((field.label == FieldDescriptorProto::LABEL_REQUIRED)  &&  ! option.no_required) {
                check_required_fields += CHECK_REQUIRED_FIELD_TEMPLATE.format(message_type.name, field.name);
            }
        }

        if(! option.no_class) {
            std::cout << CLASS_TEMPLATE.format(message_type.name, fields_defs, has_fields_defs,
                option.no_encoder? "" : ENCODER_DECLARATION_TEMPLATE,
                option.no_decoder? "" : DECODER_DECLARATION_TEMPLATE);
        }
        if(! option.no_encoder) {
            std::cout << ENCODER_TEMPLATE.format(message_type.name, encoder);
        }
        if(! option.no_decoder) {
            std::cout << DECODER_TEMPLATE.format(message_type.name, decode_cases, check_required_fields);
        }
    }
}
