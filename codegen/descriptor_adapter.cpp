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
            service.method[j].name =
                intern_text(strings, service.method[j].name);
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

} // namespace

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
