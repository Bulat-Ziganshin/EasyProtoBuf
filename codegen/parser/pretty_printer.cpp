#include "pretty_printer.hpp"

#include <cstdint>
#include <ostream>

#include "schema_semantics.hpp"

namespace easypb_proto {

// W-35: the guarded conversion lives in schema.hpp, beside the storage it
// converts; one door for every frontend.
using easypb_schema::view_text;

namespace {

// C-escape a custom JSON name for the diagnostic printer. The value is
// arbitrary text the schema may carry, so quotes, backslashes and control
// bytes must not change the printed shape.
std::string escape_json_name(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c >= 32u && c <= 126u) {
                    escaped.push_back(static_cast<char>(c));
                } else {
                    escaped.push_back('\\');
                    escaped.push_back(static_cast<char>('0' + ((c >> 6) & 7u)));
                    escaped.push_back(static_cast<char>('0' + ((c >> 3) & 7u)));
                    escaped.push_back(static_cast<char>('0' + (c & 7u)));
                }
                break;
        }
    }
    return escaped;
}

void indent(std::ostream& output, unsigned level)
{
    for (unsigned i = 0; i < level; ++i) output << "  ";
}

void print_enum(const EnumDescriptorProto& descriptor, std::ostream& output, unsigned level)
{
    indent(output, level);
    output << "enum " << view_text(descriptor.name) << " {\n";
    for (std::size_t i = 0; i < descriptor.value.size(); ++i) {
        indent(output, level + 1);
        output << view_text(descriptor.value[i].name) << " = "
               << descriptor.value[i].number << "\n";
    }
    indent(output, level);
    output << "}\n";
}

const char* label_name(std::int32_t label)
{
    if (label == FieldDescriptorProto::LABEL_REQUIRED) return "required";
    if (label == FieldDescriptorProto::LABEL_REPEATED) return "repeated";
    return "optional";
}

void print_message(const DescriptorProto& descriptor, std::ostream& output, unsigned level)
{
    indent(output, level);
    output << "message " << view_text(descriptor.name);
    if (descriptor.has_options && descriptor.options.has_map_entry &&
        descriptor.options.map_entry) {
        output << " [map_entry]";
    }
    output << " {\n";

    for (std::size_t i = 0; i < descriptor.oneof_decl.size(); ++i) {
        indent(output, level + 1);
        output << "oneof[" << i << "] "
               << view_text(descriptor.oneof_decl[i].name) << "\n";
    }
    for (std::size_t i = 0; i < descriptor.field.size(); ++i) {
        const FieldDescriptorProto& field = descriptor.field[i];
        indent(output, level + 1);
        output << label_name(field.label) << ' ';
        if (field.has_type_name) output << view_text(field.type_name);
        else output << "type#" << field.type;
        output << ' ' << view_text(field.name) << " = " << field.number;
        if (field.has_default_value) {
            output << " [default=" << view_text(field.default_value) << ']';
        }
        if (field.has_json_name) {
            const std::string name = view_text(field.name);
            const std::string json_name = view_text(field.json_name);
            if (json_name != easypb_schema::to_json_name(name)) {
                output << " [json_name=\"" << escape_json_name(json_name)
                       << "\"]";
            }
        }
        if (field.has_options && field.options.has_packed) {
            output << " [packed=" << (field.options.packed ? "true" : "false") << ']';
        }
        if (field.has_oneof_index) {
            output << " [oneof_index=" << field.oneof_index << ']';
        }
        output << '\n';
    }
    for (std::size_t i = 0; i < descriptor.enum_type.size(); ++i) {
        print_enum(descriptor.enum_type[i], output, level + 1);
    }
    for (std::size_t i = 0; i < descriptor.nested_type.size(); ++i) {
        print_message(descriptor.nested_type[i], output, level + 1);
    }
    indent(output, level);
    output << "}\n";
}

} // namespace


void print_descriptor(const FileDescriptorProto& file, std::ostream& output)
{
    output << "file " << view_text(file.name) << " syntax=";
    if (file.has_syntax) output << view_text(file.syntax);
    else output << "proto2";
    output << '\n';
    if (file.has_package) {
        output << "package " << view_text(file.package) << '\n';
    }
    for (std::size_t i = 0; i < file.enum_type.size(); ++i) {
        print_enum(file.enum_type[i], output, 0);
    }
    for (std::size_t i = 0; i < file.message_type.size(); ++i) {
        print_message(file.message_type[i], output, 0);
    }
}

void print_descriptor(const ParsedProto& parsed, std::ostream& output)
{
    print_descriptor(parsed.file, output);
    for (std::size_t i = 0; i < parsed.imports.size(); ++i) {
        output << "import ";
        if (parsed.imports[i].modifier == ImportInfo::PUBLIC_IMPORT) output << "public ";
        else if (parsed.imports[i].modifier == ImportInfo::WEAK_IMPORT) output << "weak ";
        output << parsed.imports[i].path << '\n';
    }
}

} // namespace easypb_proto
