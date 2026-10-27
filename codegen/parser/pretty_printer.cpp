#include "pretty_printer.hpp"

#include <cstdint>
#include <ostream>
#include <sstream>

#include "schema_semantics.hpp"

namespace easypb_proto {

// W-35: the guarded conversion lives in schema.hpp, beside the storage it
// converts; one door for every frontend.
using easypb_schema::view_text;

namespace {

// C-escape a quoted value for the diagnostic printer. A custom JSON name and a
// reserved name are arbitrary text the schema may carry, so quotes,
// backslashes and control bytes must not change the printed shape.
std::string escape_quoted(const std::string& value)
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

// One `reserved`/`extensions` declaration in source spelling. `last` is the
// last number the declaration covers, already normalized from the descriptor
// spelling: a message range stores an EXCLUSIVE end, an enum range an
// INCLUSIVE one, and the callers convert before they call here. A
// one-number range prints without `to`, exactly as the parser accepts it.
void print_range_line(std::ostream& output, unsigned level, const char* keyword,
                      std::int64_t first, std::int64_t last)
{
    indent(output, level);
    output << keyword << ' ' << first;
    if (last != first) output << " to " << last;
    output << ";\n";
}

// The exclusive end of a message range is printed as the last number it
// covers. The conversion runs in 64 bits so an absurd decoded end cannot
// overflow the signed 32-bit value the field stores.
std::int64_t inclusive_last(std::int32_t exclusive_end)
{
    return static_cast<std::int64_t>(exclusive_end) - 1;
}

void print_reserved_name(std::ostream& output, unsigned level,
                         const str_view& name)
{
    indent(output, level);
    output << "reserved \"" << escape_quoted(view_text(name)) << "\";\n";
}

void print_enum(const EnumDescriptorProto& descriptor, std::ostream& output, unsigned level)
{
    indent(output, level);
    output << "enum " << view_text(descriptor.name) << " {\n";
    if (descriptor.has_options && descriptor.options.has_allow_alias) {
        indent(output, level + 1);
        output << "option allow_alias = "
               << (descriptor.options.allow_alias ? "true" : "false") << ";\n";
    }
    for (std::size_t i = 0; i < descriptor.reserved_range.size(); ++i) {
        const EnumDescriptorProto_EnumReservedRange& range =
            descriptor.reserved_range[i];
        // Both ends of an enum reserved range are inclusive.
        print_range_line(output, level + 1, "reserved", range.start, range.end);
    }
    for (std::size_t i = 0; i < descriptor.reserved_name.size(); ++i) {
        print_reserved_name(output, level + 1, descriptor.reserved_name[i]);
    }
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
    // U-31: the MessageSet option is a decoded declaration the linker refuses
    // on (DIAGNOSTIC_MESSAGE_SET_WITH_FIELDS), so a dump without it misdescribes
    // the input. It is printed beside the other message-option marker.
    if (descriptor.has_options &&
        descriptor.options.has_message_set_wire_format &&
        descriptor.options.message_set_wire_format) {
        output << " [message_set_wire_format]";
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
                output << " [json_name=\"" << escape_quoted(json_name)
                       << "\"]";
            }
        }
        if (field.has_options && field.options.has_packed) {
            output << " [packed=" << (field.options.packed ? "true" : "false") << ']';
        }
        if (field.has_oneof_index) {
            output << " [oneof_index=" << field.oneof_index << ']';
        }
        // D1-A: the presence flag is part of what a descriptor dump must show.
        // Both frontends decode or synthesize it, so the source and `.pbs`
        // prints carry the same marker.
        if (field.has_proto3_optional && field.proto3_optional) {
            output << " [proto3_optional=true]";
        }
        output << '\n';
    }
    for (std::size_t i = 0; i < descriptor.extension_range.size(); ++i) {
        const DescriptorProto_ExtensionRange& range =
            descriptor.extension_range[i];
        print_range_line(output, level + 1, "extensions", range.start,
                         inclusive_last(range.end));
    }
    for (std::size_t i = 0; i < descriptor.reserved_range.size(); ++i) {
        const DescriptorProto_ReservedRange& range = descriptor.reserved_range[i];
        print_range_line(output, level + 1, "reserved", range.start,
                         inclusive_last(range.end));
    }
    for (std::size_t i = 0; i < descriptor.reserved_name.size(); ++i) {
        print_reserved_name(output, level + 1, descriptor.reserved_name[i]);
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

// CR-48: a service is a declaration the model stores and the linker
// validates, so `--print-descriptor` has to show it. A method is printed as
// its name, its endpoints and the streaming markers the model decoded; a
// unary method prints no marker, and a streaming one prints the flag that is
// set, which is the same information protoc's descriptor carries.
void print_service(const ServiceDescriptorProto& descriptor, std::ostream& output,
                   unsigned level)
{
    indent(output, level);
    output << "service " << view_text(descriptor.name) << " {\n";
    for (std::size_t i = 0; i < descriptor.method.size(); ++i) {
        const MethodDescriptorProto& method = descriptor.method[i];
        indent(output, level + 1);
        output << "rpc " << view_text(method.name) << " ("
               << view_text(method.input_type) << ") returns ("
               << view_text(method.output_type) << ')';
        if (method.client_streaming || method.server_streaming) {
            output << " [";
            if (method.client_streaming) output << "client_streaming=true";
            if (method.client_streaming && method.server_streaming) {
                output << ", ";
            }
            if (method.server_streaming) output << "server_streaming=true";
            output << ']';
        }
        output << '\n';
    }
    indent(output, level);
    output << "}\n";
}

// A decoded edition is printed by the year protobuf 30.1 names for it; any
// other value is printed as the number the set actually carries, because the
// dump reports what was read rather than guessing a name.
std::string edition_text(std::int32_t edition)
{
    if (edition == easypb_schema::kEdition2023) return "2023";
    if (edition == easypb_schema::kEdition2024) return "2024";
    std::ostringstream output;
    output << edition;
    return output.str();
}

bool index_listed(const std::vector<std::int32_t>& indices, std::size_t index)
{
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] == static_cast<std::int32_t>(index)) return true;
    }
    return false;
}

// The import modifier is carried by the file's public/weak index lists, which
// every frontend fills; the source frontend's positional import list is not
// consulted, so both doors print one spelling.
const char* import_modifier(const FileDescriptorProto& file, std::size_t index)
{
    if (index_listed(file.public_dependency, index)) return "public ";
    if (index_listed(file.weak_dependency, index)) return "weak ";
    return "";
}

} // namespace


void print_descriptor(const FileDescriptorProto& file, std::ostream& output)
{
    output << "file " << view_text(file.name) << " syntax=";
    if (file.has_syntax) output << view_text(file.syntax);
    else output << "proto2";
    if (file.has_edition) output << " edition=" << edition_text(file.edition);
    output << '\n';
    if (file.has_package) {
        output << "package " << view_text(file.package) << '\n';
    }
    // U-31: a dependency is a decoded declaration of the file, not only of the
    // source frontend, so the descriptor door prints it too; the modifier
    // comes from the public/weak index lists.
    for (std::size_t i = 0; i < file.dependency.size(); ++i) {
        output << "import " << import_modifier(file, i) << '"'
               << escape_quoted(view_text(file.dependency[i])) << "\";\n";
    }
    for (std::size_t i = 0; i < file.enum_type.size(); ++i) {
        print_enum(file.enum_type[i], output, 0);
    }
    for (std::size_t i = 0; i < file.message_type.size(); ++i) {
        print_message(file.message_type[i], output, 0);
    }
    for (std::size_t i = 0; i < file.service.size(); ++i) {
        print_service(file.service[i], output, 0);
    }
}

void print_descriptor(const ParsedProto& parsed, std::ostream& output)
{
    // The decoded file carries the authoritative dependency lists for both
    // frontends, so the source door prints the same tree as the descriptor
    // door; `parsed.imports` supplies positions, not a second string source.
    print_descriptor(parsed.file, output);
}

} // namespace easypb_proto
