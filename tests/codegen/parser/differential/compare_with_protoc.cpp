#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "proto_parser.hpp"

namespace {

std::string text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

bool read_file(const char* path, std::string& data)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    data = buffer.str();
    return input.good() || input.eof();
}

struct Comparison
{
    std::set<std::string> local_symbols;
    unsigned errors;

    Comparison() : errors(0) {}

    void fail(const std::string& path, const std::string& message)
    {
        std::cerr << path << ": " << message << '\n';
        ++errors;
    }

    void collect_enum(const EnumDescriptorProto& value, const std::string& parent)
    {
        local_symbols.insert(parent + "." + text(value.name));
    }

    void collect_message(const DescriptorProto& value, const std::string& parent)
    {
        const std::string name = parent + "." + text(value.name);
        local_symbols.insert(name);
        for (std::size_t i = 0; i < value.enum_type.size(); ++i) {
            collect_enum(value.enum_type[i], name);
        }
        for (std::size_t i = 0; i < value.nested_type.size(); ++i) {
            collect_message(value.nested_type[i], name);
        }
    }

    void collect_symbols(const FileDescriptorProto& file)
    {
        const std::string root = file.has_package && !text(file.package).empty()
            ? "." + text(file.package) : std::string();
        for (std::size_t i = 0; i < file.enum_type.size(); ++i) {
            collect_enum(file.enum_type[i], root);
        }
        for (std::size_t i = 0; i < file.message_type.size(); ++i) {
            collect_message(file.message_type[i], root);
        }
    }

    bool external_name_equivalent(const std::string& ours,
                                  const std::string& official) const
    {
        if (ours == official) return true;
        if (ours.empty() || official.empty() || ours[0] == '.') return false;
        if (official.size() <= ours.size()) return false;
        const std::size_t offset = official.size() - ours.size();
        return official[offset - 1] == '.' && official.compare(offset, ours.size(), ours) == 0;
    }

    void compare_enum(const EnumDescriptorProto& ours,
                      const EnumDescriptorProto& official,
                      const std::string& path)
    {
        if (ours.has_name != official.has_name || text(ours.name) != text(official.name)) {
            fail(path, "enum name differs");
        }
        if (ours.value.size() != official.value.size()) {
            fail(path, "enum value count differs");
            return;
        }
        for (std::size_t i = 0; i < ours.value.size(); ++i) {
            const EnumValueDescriptorProto& left = ours.value[i];
            const EnumValueDescriptorProto& right = official.value[i];
            const std::string item = path + ".value[" + number(i) + "]";
            if (left.has_name != right.has_name || text(left.name) != text(right.name)) {
                fail(item, "name differs");
            }
            if (left.has_number != right.has_number || left.number != right.number) {
                fail(item, "number differs");
            }
        }
    }

    static std::string number(std::size_t value)
    {
        std::ostringstream output;
        output << value;
        return output.str();
    }

    void compare_field(const FieldDescriptorProto& ours,
                       const FieldDescriptorProto& official,
                       const std::string& path)
    {
        if (ours.has_name != official.has_name || text(ours.name) != text(official.name)) {
            fail(path, "field name differs");
        }
        if (ours.has_number != official.has_number || ours.number != official.number) {
            fail(path, "field number differs");
        }
        if (ours.has_label != official.has_label || ours.label != official.label) {
            fail(path, "field label differs");
        }

        const std::string left_name = ours.has_type_name ? text(ours.type_name) : std::string();
        const std::string right_name = official.has_type_name ? text(official.type_name) : std::string();
        const bool local = !right_name.empty() && local_symbols.find(right_name) != local_symbols.end();

        if (ours.has_type_name != official.has_type_name) {
            fail(path, "type_name presence differs");
        } else if (ours.has_type_name) {
            const bool names_match = local ? (left_name == right_name)
                                           : external_name_equivalent(left_name, right_name);
            if (!names_match) {
                fail(path, "type_name differs: ours=" + left_name + " official=" + right_name);
            }
        }

        bool type_matches = ours.has_type == official.has_type && ours.type == official.type;
        if (!type_matches && !local && ours.has_type && official.has_type &&
            ours.type == FieldDescriptorProto::TYPE_MESSAGE &&
            official.type == FieldDescriptorProto::TYPE_ENUM) {
            // Without imported descriptors the standalone parser cannot know
            // that an unresolved external symbol is an enum rather than a message.
            type_matches = true;
        }
        if (!type_matches) fail(path, "field type differs");

        if (ours.has_default_value != official.has_default_value) {
            fail(path, "default_value presence differs");
        } else if (ours.has_default_value && text(ours.default_value) != text(official.default_value)) {
            fail(path, "default_value differs: ours=" + text(ours.default_value) +
                       " official=" + text(official.default_value));
        }

        if (ours.options.has_packed != official.options.has_packed) {
            fail(path, "packed presence differs");
        } else if (ours.options.has_packed && ours.options.packed != official.options.packed) {
            fail(path, "packed value differs");
        }

        if (ours.has_oneof_index != official.has_oneof_index) {
            fail(path, "oneof_index presence differs");
        } else if (ours.has_oneof_index && ours.oneof_index != official.oneof_index) {
            fail(path, "oneof_index differs");
        }
    }

    void compare_message(const DescriptorProto& ours,
                         const DescriptorProto& official,
                         const std::string& path)
    {
        if (ours.has_name != official.has_name || text(ours.name) != text(official.name)) {
            fail(path, "message name differs");
        }
        if (ours.options.has_map_entry != official.options.has_map_entry) {
            fail(path, "map_entry presence differs");
        } else if (ours.options.has_map_entry && ours.options.map_entry != official.options.map_entry) {
            fail(path, "map_entry value differs");
        }
        if (ours.oneof_decl.size() != official.oneof_decl.size()) {
            fail(path, "oneof count differs");
        } else {
            for (std::size_t i = 0; i < ours.oneof_decl.size(); ++i) {
                const OneofDescriptorProto& left = ours.oneof_decl[i];
                const OneofDescriptorProto& right = official.oneof_decl[i];
                if (left.has_name != right.has_name || text(left.name) != text(right.name)) {
                    fail(path + ".oneof[" + number(i) + "]", "name differs");
                }
            }
        }
        if (ours.field.size() != official.field.size()) {
            fail(path, "field count differs");
        } else {
            for (std::size_t i = 0; i < ours.field.size(); ++i) {
                compare_field(ours.field[i], official.field[i],
                              path + ".field[" + number(i) + "]");
            }
        }
        if (ours.enum_type.size() != official.enum_type.size()) {
            fail(path, "nested enum count differs");
        } else {
            for (std::size_t i = 0; i < ours.enum_type.size(); ++i) {
                compare_enum(ours.enum_type[i], official.enum_type[i],
                             path + ".enum[" + number(i) + "]");
            }
        }
        if (ours.nested_type.size() != official.nested_type.size()) {
            fail(path, "nested message count differs");
        } else {
            for (std::size_t i = 0; i < ours.nested_type.size(); ++i) {
                compare_message(ours.nested_type[i], official.nested_type[i],
                                path + ".message[" + number(i) + "]");
            }
        }
    }

    void compare_file(const FileDescriptorProto& ours, const FileDescriptorProto& official)
    {
        if (ours.has_name != official.has_name || text(ours.name) != text(official.name)) {
            fail("file", "name differs: ours=" + text(ours.name) +
                         " official=" + text(official.name));
        }
        if (ours.has_package != official.has_package || text(ours.package) != text(official.package)) {
            fail("file", "package differs");
        }
        if (ours.has_syntax != official.has_syntax ||
            (ours.has_syntax && text(ours.syntax) != text(official.syntax))) {
            fail("file", "syntax differs");
        }
        if (ours.enum_type.size() != official.enum_type.size()) {
            fail("file", "top-level enum count differs");
        } else {
            for (std::size_t i = 0; i < ours.enum_type.size(); ++i) {
                compare_enum(ours.enum_type[i], official.enum_type[i],
                             "file.enum[" + number(i) + "]");
            }
        }
        if (ours.message_type.size() != official.message_type.size()) {
            fail("file", "top-level message count differs");
        } else {
            for (std::size_t i = 0; i < ours.message_type.size(); ++i) {
                compare_message(ours.message_type[i], official.message_type[i],
                                "file.message[" + number(i) + "]");
            }
        }
    }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: parser-differential-compare SOURCE.proto OFFICIAL.pb\n";
        return EXIT_FAILURE;
    }

    std::string source;
    std::string encoded;
    if (!read_file(argv[1], source)) {
        std::cerr << "cannot read source file: " << argv[1] << '\n';
        return EXIT_FAILURE;
    }
    if (!read_file(argv[2], encoded)) {
        std::cerr << "cannot read descriptor set: " << argv[2] << '\n';
        return EXIT_FAILURE;
    }

    FileDescriptorSet official_set;
    try {
        official_set = easypb::decode<FileDescriptorSet>(encoded);
    } catch (const std::exception& error) {
        std::cerr << "cannot decode official descriptor set: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    if (official_set.file.size() != 1 || !official_set.file[0].has_name) {
        std::cerr << "expected exactly one named file in descriptor set\n";
        return EXIT_FAILURE;
    }

    easypb_proto::ParsedProto parsed;
    easypb_proto::Diagnostic error;
    const std::string official_name = text(official_set.file[0].name);
    if (!easypb_proto::parse_proto(official_name, source, parsed, error)) {
        std::cerr << error.file << ':' << error.location.line << ':' << error.location.column
                  << ": " << error.message << '\n';
        return EXIT_FAILURE;
    }

    Comparison comparison;
    comparison.collect_symbols(official_set.file[0]);
    comparison.compare_file(parsed.file, official_set.file[0]);
    if (comparison.errors != 0) {
        std::cerr << comparison.errors << " descriptor difference(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << official_name << ": MATCH\n";
    return EXIT_SUCCESS;
}
