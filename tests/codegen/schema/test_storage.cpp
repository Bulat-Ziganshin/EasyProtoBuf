// Tests for the parser-independent schema storage module
// (codegen/schema.hpp/.cpp): StringPool, SchemaFile, SchemaSet, descriptor
// dependency decoding, and descriptor paths. This test links only
// easypb_schema and must keep passing in descriptor-set-only builds.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "schema.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition) {
        std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __FILE__, __LINE__)

std::string text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

void test_string_pool_save_and_clear()
{
    easypb_schema::StringPool pool;
    const str_view first = pool.save("alpha");
    const str_view second = pool.save("beta beta");
    CHECK(text(first) == "alpha");
    CHECK(text(second) == "beta beta");
    CHECK(first.data() != second.data());

    // Grow the pool past its initial block and check earlier strings stay put.
    const char* first_data = first.data();
    for (int i = 0; i < 2000; ++i) {
        pool.save("padding text to force more blocks");
    }
    CHECK(text(first) == "alpha");
    CHECK(first.data() == first_data);

    pool.clear();
    const str_view after = pool.save("gamma");
    CHECK(text(after) == "gamma");
}

void test_schema_file_defaults_to_source_origin()
{
    easypb_schema::SchemaFile file;
    CHECK(!file.from_descriptor_set);
    CHECK(file.physical_name.empty());
    CHECK(file.imports.empty());
    CHECK(file.warnings.empty());
    CHECK(file.field_sources.empty());
    CHECK(file.declaration_locations.empty());
    CHECK(file.edges.empty());

    file.physical_name = "root.proto";
    file.from_descriptor_set = true;
    file.clear();
    CHECK(!file.from_descriptor_set);
    CHECK(file.physical_name.empty());
    CHECK(!file.file.has_name);
}

void test_schema_set_ownership_and_targets()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& first = set.add_file("b.proto");
    easypb_schema::SchemaFile& second = set.add_file("a.proto");
    CHECK(set.find_file("a.proto") == &second);
    CHECK(set.find_file("b.proto") == &first);
    CHECK(set.find_file("missing.proto") == 0);
    CHECK(text(first.file.name) == "b.proto");
    CHECK(first.physical_name == "b.proto");

    bool duplicate_rejected = false;
    try {
        set.add_file("a.proto");
    } catch (const std::exception&) {
        duplicate_rejected = true;
    }
    CHECK(duplicate_rejected);

    // Targets deduplicate in first-requested order.
    set.add_target(second);
    set.add_target(first);
    set.add_target(second);
    CHECK(set.targets().size() == 2);
    if (set.targets().size() == 2) {
        CHECK(set.targets()[0] == &second);
        CHECK(set.targets()[1] == &first);
    }

    // Addresses stay stable while the owning vector grows.
    easypb_schema::SchemaFile* before = set.find_file("b.proto");
    for (int i = 0; i < 100; ++i) {
        char name[32];
        std::sprintf(name, "generated-%d.proto", i);
        set.add_file(name);
    }
    CHECK(set.find_file("b.proto") == before);
    CHECK(set.files().size() == 102);

    set.clear();
    CHECK(set.find_file("a.proto") == 0);
    CHECK(set.files().empty());
    CHECK(set.targets().empty());
}

void test_decode_dependency_lists_unpacked()
{
    // dependency = ["a.proto", "b.proto"], public = [1], weak = [0],
    // each index encoded as a separate varint entry.
    const unsigned char encoded[] = {
        0x1a, 0x07, 'a', '.', 'p', 'r', 'o', 't', 'o',
        0x1a, 0x07, 'b', '.', 'p', 'r', 'o', 't', 'o',
        0x50, 0x01,
        0x58, 0x00
    };
    const std::string buffer(reinterpret_cast<const char*>(encoded), sizeof(encoded));
    const FileDescriptorProto file = easypb::decode<FileDescriptorProto>(buffer);
    CHECK(file.dependency.size() == 2);
    if (file.dependency.size() == 2) {
        CHECK(text(file.dependency[0]) == "a.proto");
        CHECK(text(file.dependency[1]) == "b.proto");
    }
    CHECK(file.public_dependency.size() == 1);
    if (!file.public_dependency.empty()) CHECK(file.public_dependency[0] == 1);
    CHECK(file.weak_dependency.size() == 1);
    if (!file.weak_dependency.empty()) CHECK(file.weak_dependency[0] == 0);
}

void test_decode_dependency_lists_packed()
{
    // public_dependency = [0, 2] and weak_dependency = [1] in packed
    // length-delimited form.
    const unsigned char encoded[] = {
        0x1a, 0x07, 'a', '.', 'p', 'r', 'o', 't', 'o',
        0x1a, 0x07, 'b', '.', 'p', 'r', 'o', 't', 'o',
        0x1a, 0x07, 'c', '.', 'p', 'r', 'o', 't', 'o',
        0x52, 0x02, 0x00, 0x02,
        0x5a, 0x01, 0x01
    };
    const std::string buffer(reinterpret_cast<const char*>(encoded), sizeof(encoded));
    const FileDescriptorProto file = easypb::decode<FileDescriptorProto>(buffer);
    CHECK(file.dependency.size() == 3);
    CHECK(file.public_dependency.size() == 2);
    if (file.public_dependency.size() == 2) {
        CHECK(file.public_dependency[0] == 0);
        CHECK(file.public_dependency[1] == 2);
    }
    CHECK(file.weak_dependency.size() == 1);
    if (!file.weak_dependency.empty()) CHECK(file.weak_dependency[0] == 1);
}

void test_decode_service_names()
{
    // service = [{name: "Greeter"}, {name: "Worker"}].
    const unsigned char encoded[] = {
        0x32, 0x09, 0x0a, 0x07, 'G', 'r', 'e', 'e', 't', 'e', 'r',
        0x32, 0x08, 0x0a, 0x06, 'W', 'o', 'r', 'k', 'e', 'r'
    };
    const std::string buffer(reinterpret_cast<const char*>(encoded), sizeof(encoded));
    const FileDescriptorProto file = easypb::decode<FileDescriptorProto>(buffer);
    CHECK(file.service.size() == 2);
    if (file.service.size() == 2) {
        CHECK(file.service[0].has_name && text(file.service[0].name) == "Greeter");
        CHECK(file.service[1].has_name && text(file.service[1].name) == "Worker");
    }
}

easypb_schema::SchemaFile* make_message_fixture()
{
    // Caller owns the file: a package with one message holding one field and
    // one nested message holding one field, plus a top-level enum.
    easypb_schema::SchemaFile* file = new easypb_schema::SchemaFile();
    file->file.package = file->strings.save("pkg");
    file->file.has_package = true;

    DescriptorProto message;
    message.name = file->strings.save("Outer");
    message.has_name = true;
    FieldDescriptorProto field;
    field.name = file->strings.save("value");
    field.has_name = true;
    field.number = 1;
    field.has_number = true;
    field.label = FieldDescriptorProto::LABEL_OPTIONAL;
    field.has_label = true;
    field.type = FieldDescriptorProto::TYPE_INT32;
    field.has_type = true;
    message.field.push_back(field);

    DescriptorProto nested;
    nested.name = file->strings.save("Inner");
    nested.has_name = true;
    FieldDescriptorProto nested_field;
    nested_field.name = file->strings.save("text");
    nested_field.has_name = true;
    nested_field.number = 1;
    nested_field.has_number = true;
    nested_field.label = FieldDescriptorProto::LABEL_OPTIONAL;
    nested_field.has_label = true;
    nested_field.type = FieldDescriptorProto::TYPE_STRING;
    nested_field.has_type = true;
    nested.field.push_back(nested_field);
    message.nested_type.push_back(nested);
    file->file.message_type.push_back(message);

    EnumDescriptorProto enumeration;
    enumeration.name = file->strings.save("State");
    enumeration.has_name = true;
    EnumValueDescriptorProto value;
    value.name = file->strings.save("READY");
    value.has_name = true;
    value.number = 7;
    value.has_number = true;
    enumeration.value.push_back(value);
    file->file.enum_type.push_back(enumeration);
    return file;
}

void test_descriptor_path_lookups()
{
    easypb_schema::SchemaFile* file = make_message_fixture();

    using easypb_schema::DescriptorPath;
    const DescriptorPath outer = easypb_schema::file_message_path(0);
    const DescriptorPath value = easypb_schema::message_field_path(outer, 0);
    const DescriptorPath inner = easypb_schema::message_nested_path(outer, 0);
    const DescriptorPath inner_text = easypb_schema::message_field_path(inner, 0);
    const DescriptorPath state = easypb_schema::file_enum_path(0);
    const DescriptorPath ready = easypb_schema::enum_value_path(state, 0);

    CHECK(easypb_schema::find_message(*file, outer) != 0);
    CHECK(easypb_schema::find_message(*file, inner) != 0);
    const FieldDescriptorProto* found_value =
        easypb_schema::find_field(*file, value);
    CHECK(found_value != 0);
    CHECK(found_value && text(found_value->name) == "value");
    const FieldDescriptorProto* found_text =
        easypb_schema::find_field(*file, inner_text);
    CHECK(found_text != 0);
    CHECK(found_text && text(found_text->name) == "text");
    const EnumDescriptorProto* found_enum = easypb_schema::find_enum(*file, state);
    CHECK(found_enum != 0);
    CHECK(found_enum && text(found_enum->name) == "State");

    // Every lookup validates before dereferencing it.
    const DescriptorPath missing_message = easypb_schema::file_message_path(4);
    CHECK(easypb_schema::find_message(*file, missing_message) == 0);
    const DescriptorPath missing_field =
        easypb_schema::message_field_path(outer, 9);
    CHECK(easypb_schema::find_field(*file, missing_field) == 0);
    const DescriptorPath wrong_kind =
        easypb_schema::message_field_path(outer, 0);
    CHECK(easypb_schema::find_message(*file, wrong_kind) == 0);
    DescriptorPath truncated;
    truncated.push_back(4);
    CHECK(easypb_schema::find_message(*file, truncated) == 0);
    CHECK(easypb_schema::find_field_source(*file, value) == 0);
    (void)ready;

    delete file;
}

} // namespace

int main()
{
    test_string_pool_save_and_clear();
    test_schema_file_defaults_to_source_origin();
    test_schema_set_ownership_and_targets();
    test_decode_dependency_lists_unpacked();
    test_decode_dependency_lists_packed();
    test_decode_service_names();
    test_descriptor_path_lookups();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all schema storage tests passed\n";
    return EXIT_SUCCESS;
}
