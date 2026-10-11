#include "descriptor_adapter.hpp"

#include <cstddef>
#include <set>
#include <string>

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
