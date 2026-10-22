#include "descriptor_adapter.hpp"

#include <cstddef>
#include <set>
#include <sstream>
#include <string>

#include "schema_semantics.hpp"

namespace easypb_schema {
namespace {

// One string into the owning pool. An empty value stays an empty view:
// saving it would allocate a block for nothing, and no reader distinguishes
// an interned empty string from an absent one.
str_view intern_text(StringPool& strings, str_view value)
{
    if (value.empty()) return str_view();
    return strings.save(value.data(), value.size());
}

void intern_enum(StringPool& strings, EnumDescriptorProto& enum_type)
{
    enum_type.name = intern_text(strings, enum_type.name);
    for (std::size_t i = 0; i < enum_type.value.size(); ++i) {
        enum_type.value[i].name =
            intern_text(strings, enum_type.value[i].name);
    }
}

void intern_message(StringPool& strings, DescriptorProto& message)
{
    message.name = intern_text(strings, message.name);
    for (std::size_t i = 0; i < message.field.size(); ++i) {
        FieldDescriptorProto& field = message.field[i];
        field.name = intern_text(strings, field.name);
        field.type_name = intern_text(strings, field.type_name);
        field.default_value = intern_text(strings, field.default_value);
        field.options.cpp.type = intern_text(strings, field.options.cpp.type);
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        intern_message(strings, message.nested_type[i]);
    }
    for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
        intern_enum(strings, message.enum_type[i]);
    }
    for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
        message.oneof_decl[i].name =
            intern_text(strings, message.oneof_decl[i].name);
    }
}

// Every str_view the descriptor model retains, in one walk. The set is
// closed over descriptor.pb.hpp: a field added there without a line here
// would keep pointing into a caller buffer this library does not own, so
// the D2 lifetime test enumerates the same list.
void intern_file_descriptor(StringPool& strings, FileDescriptorProto& file)
{
    file.name = intern_text(strings, file.name);
    file.package = intern_text(strings, file.package);
    file.syntax = intern_text(strings, file.syntax);
    for (std::size_t i = 0; i < file.dependency.size(); ++i) {
        file.dependency[i] = intern_text(strings, file.dependency[i]);
    }
    for (std::size_t i = 0; i < file.message_type.size(); ++i) {
        intern_message(strings, file.message_type[i]);
    }
    for (std::size_t i = 0; i < file.enum_type.size(); ++i) {
        intern_enum(strings, file.enum_type[i]);
    }
    for (std::size_t i = 0; i < file.service.size(); ++i) {
        ServiceDescriptorProto& service = file.service[i];
        service.name = intern_text(strings, service.name);
        for (std::size_t j = 0; j < service.method.size(); ++j) {
            MethodDescriptorProto& method = service.method[j];
            method.name = intern_text(strings, method.name);
            method.input_type = intern_text(strings, method.input_type);
            method.output_type = intern_text(strings, method.output_type);
        }
    }
}

// The one shared deep-copy helper: the tree is copied whole, then every
// retained string is re-pointed into the owning pool. A shallow assignment
// would leave the copy pointing into the caller's buffer, which is exactly
// the defect this helper exists to prevent (D1/D2).
void copy_descriptor(const FileDescriptorProto& source, SchemaFile& slot)
{
    slot.file = source;
    intern_file_descriptor(slot.strings, slot.file);
}

std::string own_text(str_view value)
{
    if (value.empty()) return std::string();
    return std::string(value.data(), value.size());
}

// True when the byte is a C0 control character. A logical Protobuf file name
// never carries one: the name is a path the user spelled, and the generator
// writes it into the "// Source:" line of the header, so a newline in it
// would inject arbitrary lines into the generated file.
bool is_control_character(char c)
{
    const unsigned char byte = static_cast<unsigned char>(c);
    return byte < 0x20u;
}

// The control character the name carries, spelled for a diagnostic.
std::string control_character_name(char c)
{
    const unsigned char byte = static_cast<unsigned char>(c);
    switch (byte) {
        case 0x00: return "NUL";
        case 0x09: return "TAB";
        case 0x0A: return "LF";
        case 0x0D: return "CR";
        default: break;
    }
    std::ostringstream text;
    text << "0x" << std::hex << static_cast<unsigned>(byte);
    return text.str();
}

bool fail(const std::string& label, const std::string& message,
          DiagnosticCode code, SchemaSet& result, Diagnostic& error)
{
    result.clear();
    error = Diagnostic();
    error.file = label;
    error.location = unknown_location();
    error.message = message;
    error.code = code;
    return false;
}

// --- structural equality of two modeled descriptors -------------------------
//
// `W-36` makes two containers that carry the same logical file a merge instead
// of a blanket refusal. "The same file" is decided over the modeled content:
// every field descriptor.pb.hpp declares, presence flags included, because a
// presence flag changes what the linker reads (an absent label is OPTIONAL, an
// absent kind is resolved by protoc's default). Strings compare by value, so
// the comparison is independent of which pool owns them.
//
// The walk must grow with the model: a field added to descriptor.pb.hpp and
// not compared here would make two different files look identical. The
// unit suite pins that with the identity fixture of the loader lane.

bool same_descriptor(const CppFieldOptions& left, const CppFieldOptions& right)
{
    return left.type == right.type && left.has_type == right.has_type;
}

bool same_descriptor(const FieldOptions& left, const FieldOptions& right)
{
    return left.packed == right.packed &&
           left.has_packed == right.has_packed && left.cpp.type == right.cpp.type &&
           left.cpp.has_type == right.cpp.has_type &&
           left.has_cpp == right.has_cpp;
}

bool same_descriptor(const FieldDescriptorProto& left,
                     const FieldDescriptorProto& right)
{
    return left.name == right.name && left.number == right.number &&
           left.label == right.label && left.type == right.type &&
           left.type_name == right.type_name &&
           left.default_value == right.default_value &&
           left.oneof_index == right.oneof_index &&
           same_descriptor(left.options, right.options) &&
           left.has_name == right.has_name &&
           left.has_number == right.has_number &&
           left.has_label == right.has_label &&
           left.has_type == right.has_type &&
           left.has_type_name == right.has_type_name &&
           left.has_default_value == right.has_default_value &&
           left.has_options == right.has_options &&
           left.has_oneof_index == right.has_oneof_index;
}

bool same_descriptor(const MessageOptions& left, const MessageOptions& right)
{
    return left.map_entry == right.map_entry &&
           left.has_map_entry == right.has_map_entry;
}

bool same_descriptor(const DescriptorProto_ExtensionRange& left,
                     const DescriptorProto_ExtensionRange& right)
{
    return left.start == right.start && left.end == right.end &&
           left.has_start == right.has_start && left.has_end == right.has_end;
}

bool same_descriptor(const EnumValueDescriptorProto& left,
                     const EnumValueDescriptorProto& right)
{
    return left.name == right.name && left.number == right.number &&
           left.has_name == right.has_name && left.has_number == right.has_number;
}

bool same_descriptor(const EnumOptions& left, const EnumOptions& right)
{
    return left.allow_alias == right.allow_alias &&
           left.has_allow_alias == right.has_allow_alias;
}

bool same_descriptor(const EnumDescriptorProto& left,
                     const EnumDescriptorProto& right)
{
    if (left.name != right.name || left.has_name != right.has_name ||
        left.has_options != right.has_options ||
        !same_descriptor(left.options, right.options) ||
        left.value.size() != right.value.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.value.size(); ++i) {
        if (!same_descriptor(left.value[i], right.value[i])) return false;
    }
    return true;
}

bool same_descriptor(const OneofDescriptorProto& left,
                     const OneofDescriptorProto& right)
{
    return left.name == right.name && left.has_name == right.has_name;
}

bool same_descriptor(const DescriptorProto& left, const DescriptorProto& right)
{
    if (left.name != right.name || left.has_name != right.has_name ||
        left.has_options != right.has_options ||
        left.extension_count != right.extension_count ||
        !same_descriptor(left.options, right.options) ||
        left.field.size() != right.field.size() ||
        left.nested_type.size() != right.nested_type.size() ||
        left.enum_type.size() != right.enum_type.size() ||
        left.oneof_decl.size() != right.oneof_decl.size() ||
        left.extension_range.size() != right.extension_range.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.field.size(); ++i) {
        if (!same_descriptor(left.field[i], right.field[i])) return false;
    }
    for (std::size_t i = 0; i < left.nested_type.size(); ++i) {
        if (!same_descriptor(left.nested_type[i], right.nested_type[i])) return false;
    }
    for (std::size_t i = 0; i < left.enum_type.size(); ++i) {
        if (!same_descriptor(left.enum_type[i], right.enum_type[i])) return false;
    }
    for (std::size_t i = 0; i < left.oneof_decl.size(); ++i) {
        if (!same_descriptor(left.oneof_decl[i], right.oneof_decl[i])) return false;
    }
    for (std::size_t i = 0; i < left.extension_range.size(); ++i) {
        if (!same_descriptor(left.extension_range[i], right.extension_range[i])) {
            return false;
        }
    }
    return true;
}

bool same_descriptor(const MethodDescriptorProto& left,
                     const MethodDescriptorProto& right)
{
    return left.name == right.name && left.input_type == right.input_type &&
           left.output_type == right.output_type && left.has_name == right.has_name &&
           left.has_input_type == right.has_input_type &&
           left.has_output_type == right.has_output_type;
}

bool same_descriptor(const ServiceDescriptorProto& left,
                     const ServiceDescriptorProto& right)
{
    if (left.name != right.name || left.has_name != right.has_name ||
        left.method.size() != right.method.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.method.size(); ++i) {
        if (!same_descriptor(left.method[i], right.method[i])) return false;
    }
    return true;
}

} // namespace

bool same_descriptor_file(const FileDescriptorProto& left,
                          const FileDescriptorProto& right)
{
    if (left.name != right.name || left.package != right.package ||
        left.syntax != right.syntax || left.edition != right.edition ||
        left.has_name != right.has_name ||
        left.has_package != right.has_package ||
        left.has_syntax != right.has_syntax ||
        left.has_edition != right.has_edition ||
        left.dependency.size() != right.dependency.size() ||
        left.public_dependency != right.public_dependency ||
        left.weak_dependency != right.weak_dependency ||
        left.message_type.size() != right.message_type.size() ||
        left.enum_type.size() != right.enum_type.size() ||
        left.service.size() != right.service.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.dependency.size(); ++i) {
        if (left.dependency[i] != right.dependency[i]) return false;
    }
    for (std::size_t i = 0; i < left.message_type.size(); ++i) {
        if (!same_descriptor(left.message_type[i], right.message_type[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < left.enum_type.size(); ++i) {
        if (!same_descriptor(left.enum_type[i], right.enum_type[i])) return false;
    }
    for (std::size_t i = 0; i < left.service.size(); ++i) {
        if (!same_descriptor(left.service[i], right.service[i])) return false;
    }
    return true;
}

bool append_descriptor_files(const std::vector<FileDescriptorProto>& files,
                             const std::string& diagnostic_label,
                             SchemaSet& result, Diagnostic& error)
{
    if (files.empty()) {
        return fail(diagnostic_label,
                    "descriptor source \"" + diagnostic_label +
                        "\" contains no FileDescriptorProto",
                    DIAGNOSTIC_GENERIC, result, error);
    }

    // Validate every name before anything is appended, so a failed call
    // never leaves a partial batch behind.
    std::set<str_view> names;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const str_view& raw_name = files[i].name;
        if (raw_name.empty()) {
            return fail(diagnostic_label,
                        "descriptor file #" +
                            easypb::detail::number_to_string(
                                static_cast<uint64_t>(i)) +
                            " in \"" + diagnostic_label +
                            "\" has no logical name; only the single-file "
                            "compatibility path may ingest an unnamed "
                            "descriptor",
                        DIAGNOSTIC_GENERIC, result, error);
        }
        const std::string logical = own_text(raw_name);
        if (!names.insert(raw_name).second || result.find_file(logical) != 0) {
            return fail(diagnostic_label,
                        "duplicate logical file name \"" + logical +
                            "\" in descriptor source \"" + diagnostic_label +
                            "\" (a file with this name was already appended)",
                        DIAGNOSTIC_DUPLICATE_SYMBOL, result, error);
        }
        // The name is a logical Protobuf name, and it reaches the "// Source:"
        // line of the generated header. A control character in it would
        // inject arbitrary lines into that header, so the name is refused
        // here - at the same place the empty and the duplicate names are
        // refused, and before anything is appended, so a failed call never
        // leaves a partial batch behind.
        for (std::size_t position = 0; position < logical.size(); ++position) {
            if (!is_control_character(logical[position])) continue;
            return fail(diagnostic_label,
                        "logical file name \"" + logical +
                            "\" in descriptor source \"" + diagnostic_label +
                            "\" carries a control character (" +
                            control_character_name(logical[position]) +
                            " at position " +
                            easypb::detail::number_to_string(
                                static_cast<uint64_t>(position)) +
                            "); a logical name cannot contain one",
                        DIAGNOSTIC_GENERIC, result, error);
        }
        // The nesting of the messages the file declares is bounded here, before
        // the file is copied: the copy interns every string of the tree and
        // walks it recursively (intern_message below), so a descriptor that
        // nests deeper than the limit would overflow the stack inside the
        // copy - and the copy is what a library caller hands this project.
        // The rule is the reference compiler's own (descriptor.h:3045) and the
        // sentence is the one it uses.
        if (!easypb_schema::validate_message_nesting_depth(files[i], logical,
                                                            error)) {
            result.clear();
            return false;
        }
    }

    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string logical = own_text(files[i].name);
        SchemaFile& slot = result.add_file(logical);
        copy_descriptor(files[i], slot);
        // Descriptor origin is a property of how the bytes were obtained,
        // not of where a container happened to live; a file wrapper may
        // retain the container path in physical_name afterwards.
        slot.physical_name.clear();
        slot.from_descriptor_set = true;
    }
    return true;
}

} // namespace easypb_schema
