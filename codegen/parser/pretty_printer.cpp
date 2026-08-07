#include "pretty_printer.hpp"

#include <cstdint>
#include <ostream>

namespace easypb_proto {

namespace {

std::string view_text(const str_view& value)
{
    return std::string(value.data(), value.size());
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
