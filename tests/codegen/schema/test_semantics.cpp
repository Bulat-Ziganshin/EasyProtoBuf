// Tests for the parser-independent semantic module
// (codegen/schema_semantics.hpp/.cpp): symbol indexing and local/strict
// field resolution. Descriptors are built by hand so this test links only
// easypb_schema and keeps passing in descriptor-set-only builds.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

#include "schema_semantics.hpp"
#include "schema_semantics_internal.hpp"

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

easypb_schema::SourceLocation at_line(int line)
{
    easypb_schema::SourceLocation location;
    location.offset = 0;
    location.line = static_cast<std::size_t>(line);
    location.column = 1;
    return location;
}

void record_source(easypb_schema::SchemaFile& file,
                   const easypb_schema::DescriptorPath& path,
                   const char* raw_type_name, int type_line,
                   bool has_default, int default_line,
                   bool has_packed, int packed_line)
{
    easypb_schema::FieldSource source;
    source.path = path;
    if (raw_type_name != 0) source.raw_type_name = raw_type_name;
    source.type_location =
        type_line > 0 ? at_line(type_line) : easypb_schema::unknown_location();
    source.default_location =
        default_line > 0 ? at_line(default_line) : easypb_schema::unknown_location();
    source.packed_location =
        packed_line > 0 ? at_line(packed_line) : easypb_schema::unknown_location();
    (void)has_default;
    (void)has_packed;
    file.field_sources.push_back(source);
}

void declare(easypb_schema::SchemaFile& file,
             const easypb_schema::DescriptorPath& path, int line)
{
    file.declaration_locations[path] = at_line(line);
}

FieldDescriptorProto make_named_field(easypb_schema::SchemaFile& file,
                                      const char* name, int number,
                                      const char* raw_type_name,
                                      bool repeated)
{
    FieldDescriptorProto field;
    field.name = file.strings.save(name);
    field.has_name = true;
    field.number = number;
    field.has_number = true;
    field.label = repeated ? FieldDescriptorProto::LABEL_REPEATED
                           : FieldDescriptorProto::LABEL_OPTIONAL;
    field.has_label = true;
    // Standalone placeholder: the resolution pass overwrites the kind.
    field.type = FieldDescriptorProto::TYPE_MESSAGE;
    field.has_type = true;
    field.type_name = file.strings.save(raw_type_name);
    field.has_type_name = true;
    return field;
}

FieldDescriptorProto make_scalar_field(easypb_schema::SchemaFile& file,
                                       const char* name, int number, int type)
{
    FieldDescriptorProto field;
    field.name = file.strings.save(name);
    field.has_name = true;
    field.number = number;
    field.has_number = true;
    field.label = FieldDescriptorProto::LABEL_OPTIONAL;
    field.has_label = true;
    field.type = type;
    field.has_type = true;
    return field;
}

// package p; enum E { A = 0; B = 1; }
// message M { optional E e = 1 [default = B]; optional int32 n = 2; }
easypb_schema::SchemaFile* make_local_enum_fixture(const char* default_value)
{
    easypb_schema::SchemaFile* file = new easypb_schema::SchemaFile();
    file->file.package = file->strings.save("p");
    file->file.has_package = true;

    EnumDescriptorProto enumeration;
    enumeration.name = file->strings.save("E");
    enumeration.has_name = true;
    const char* values[] = {"A", "B"};
    for (int i = 0; i < 2; ++i) {
        EnumValueDescriptorProto value;
        value.name = file->strings.save(values[i]);
        value.has_name = true;
        value.number = i;
        value.has_number = true;
        enumeration.value.push_back(value);
        declare(*file,
                easypb_schema::enum_value_path(
                    easypb_schema::file_enum_path(0), i),
                10 + i);
    }
    file->file.enum_type.push_back(enumeration);
    declare(*file, easypb_schema::file_enum_path(0), 9);

    DescriptorProto message;
    message.name = file->strings.save("M");
    message.has_name = true;
    FieldDescriptorProto field = make_named_field(*file, "e", 1, "E", false);
    field.default_value = file->strings.save(default_value);
    field.has_default_value = true;
    message.field.push_back(field);
    message.field.push_back(make_scalar_field(*file, "n", 2,
                                              FieldDescriptorProto::TYPE_INT32));
    file->file.message_type.push_back(message);

    const easypb_schema::DescriptorPath message_path =
        easypb_schema::file_message_path(0);
    declare(*file, message_path, 20);
    const easypb_schema::DescriptorPath field_path =
        easypb_schema::message_field_path(message_path, 0);
    declare(*file, field_path, 21);
    record_source(*file, field_path, "E", 21, true, 22, false, 0);
    return file;
}

void test_index_and_local_resolution()
{
    easypb_schema::SchemaFile* file = make_local_enum_fixture("B");
    easypb_schema::SymbolIndex symbols;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(*file, symbols, error));
    CHECK(symbols.symbols.find(".p.E") != symbols.symbols.end());
    CHECK(symbols.symbols.find(".p.M") != symbols.symbols.end());

    std::set<const easypb_schema::SchemaFile*> visible;
    visible.insert(file);
    CHECK(easypb_schema::resolve_file(*file, symbols, visible, true, error));
    CHECK(file->warnings.empty());
    const FieldDescriptorProto& field = file->file.message_type[0].field[0];
    CHECK(field.has_type && field.type == FieldDescriptorProto::TYPE_ENUM);
    CHECK(text(field.type_name) == ".p.E");
    delete file;
}

void test_invalid_local_enum_default_fails_at_default_location()
{
    easypb_schema::SchemaFile* file = make_local_enum_fixture("C");
    easypb_schema::SymbolIndex symbols;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(*file, symbols, error));

    std::set<const easypb_schema::SchemaFile*> visible;
    visible.insert(file);
    CHECK(!easypb_schema::resolve_file(*file, symbols, visible, true, error));
    CHECK(error.message.find("no value named C") != std::string::npos);
    CHECK(error.location.line == 22);
    delete file;
}

void test_packed_rules_need_a_known_kind()
{
    // A packed local message field is rejected at its packed location.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        DescriptorProto nested;
        nested.name = file.strings.save("N");
        nested.has_name = true;
        message.nested_type.push_back(nested);
        FieldDescriptorProto field = make_named_field(file, "n", 1, "N", true);
        field.options.packed = true;
        field.options.has_packed = true;
        field.has_options = true;
        message.field.push_back(field);
        file.file.message_type.push_back(message);
        const easypb_schema::DescriptorPath message_path =
            easypb_schema::file_message_path(0);
        const easypb_schema::DescriptorPath field_path =
            easypb_schema::message_field_path(message_path, 0);
        record_source(file, field_path, "N", 4, false, 0, true, 5);

        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::index_file(file, symbols, error));
        std::set<const easypb_schema::SchemaFile*> visible;
        visible.insert(&file);
        CHECK(!easypb_schema::resolve_file(file, symbols, visible, true, error));
        CHECK(error.message.find("packed option is valid only") != std::string::npos);
        CHECK(error.location.line == 5);
    }
    // An unresolved imported enum with explicit packed encoding is deferred
    // instead of incorrectly rejected: packed rules require a known kind.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        FieldDescriptorProto field = make_named_field(file, "states", 1, "State", true);
        field.options.packed = true;
        field.options.has_packed = true;
        field.has_options = true;
        message.field.push_back(field);
        file.file.message_type.push_back(message);
        const easypb_schema::DescriptorPath field_path =
            easypb_schema::message_field_path(
                easypb_schema::file_message_path(0), 0);
        record_source(file, field_path, "State", 4, false, 0, true, 5);

        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::index_file(file, symbols, error));
        std::set<const easypb_schema::SchemaFile*> visible;
        visible.insert(&file);
        CHECK(easypb_schema::resolve_file(file, symbols, visible, true, error));
        CHECK(file.warnings.size() == 1);
        CHECK(file.warnings[0].code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        // Repeated passes must not duplicate the warning.
        CHECK(easypb_schema::resolve_file(file, symbols, visible, true, error));
        CHECK(file.warnings.size() == 1);
    }
    // D-51: `[packed = false]` is a VALUE, not a refusal. The parsed
    // representation carries has_packed = true with packed = false, and that is
    // legal on a singular field and on a repeated string - the exact shape
    // protoc writes into the descriptor it emits.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        FieldDescriptorProto optional_field =
            make_scalar_field(file, "a", 1, FieldDescriptorProto::TYPE_INT32);
        optional_field.options.packed = false;
        optional_field.options.has_packed = true;
        optional_field.has_options = true;
        message.field.push_back(optional_field);
        FieldDescriptorProto repeated_string =
            make_scalar_field(file, "s", 2, FieldDescriptorProto::TYPE_STRING);
        repeated_string.label = FieldDescriptorProto::LABEL_REPEATED;
        repeated_string.options.packed = false;
        repeated_string.options.has_packed = true;
        repeated_string.has_options = true;
        message.field.push_back(repeated_string);
        file.file.message_type.push_back(message);

        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::index_file(file, symbols, error));
        std::set<const easypb_schema::SchemaFile*> visible;
        visible.insert(&file);
        CHECK(easypb_schema::resolve_file(file, symbols, visible, true, error));
    }
}

void test_repeated_resolution_warns_once_per_field()
{
    easypb_schema::SchemaFile file;
    DescriptorProto message;
    message.name = file.strings.save("M");
    message.has_name = true;
    message.field.push_back(make_named_field(file, "a", 1, "State", false));
    message.field.push_back(make_named_field(file, "b", 2, "State", false));
    file.file.message_type.push_back(message);
    const easypb_schema::DescriptorPath message_path =
        easypb_schema::file_message_path(0);
    record_source(file, easypb_schema::message_field_path(message_path, 0),
                  "State", 3, false, 0, false, 0);
    record_source(file, easypb_schema::message_field_path(message_path, 1),
                  "State", 4, false, 0, false, 0);

    easypb_schema::SymbolIndex symbols;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(file, symbols, error));
    std::set<const easypb_schema::SchemaFile*> visible;
    visible.insert(&file);
    CHECK(easypb_schema::resolve_file(file, symbols, visible, true, error));
    CHECK(file.warnings.size() == 2);
    // A second pass over the same file must not duplicate either warning.
    CHECK(easypb_schema::resolve_file(file, symbols, visible, true, error));
    CHECK(file.warnings.size() == 2);
}

void test_cross_file_service_collision_names_symbol()
{
    easypb_schema::SchemaFile first;
    first.file.package = first.strings.save("p");
    first.file.has_package = true;
    ServiceDescriptorProto service;
    service.name = first.strings.save("Worker");
    service.has_name = true;
    first.file.service.push_back(service);

    easypb_schema::SchemaFile second;
    second.file.package = second.strings.save("p");
    second.file.has_package = true;
    ServiceDescriptorProto other;
    other.name = second.strings.save("Worker");
    other.has_name = true;
    second.file.service.push_back(other);

    easypb_schema::SymbolIndex symbols;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(first, symbols, error));
    CHECK(!easypb_schema::index_file(second, symbols, error));
    CHECK(error.message.find("duplicate symbol name .p.Worker") != std::string::npos);
}

void test_descriptor_only_warnings_stay_per_field()
{
    // Without FieldSources (descriptor-only input) two fields share one
    // unknown source position; their descriptor paths still tell them apart.
    easypb_schema::SchemaFile file;
    DescriptorProto message;
    message.name = file.strings.save("M");
    message.has_name = true;
    message.field.push_back(make_named_field(file, "a", 1, "State", false));
    message.field.push_back(make_named_field(file, "b", 2, "State", false));
    file.file.message_type.push_back(message);

    easypb_schema::SymbolIndex symbols;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(file, symbols, error));
    std::set<const easypb_schema::SchemaFile*> visible;
    visible.insert(&file);
    CHECK(easypb_schema::resolve_file(file, symbols, visible, true, error));
    CHECK(file.warnings.size() == 2);
    if (file.warnings.size() == 2) {
        CHECK(file.warnings[0].message.find("[4, 0, 2, 0]") != std::string::npos);
        CHECK(file.warnings[1].message.find("[4, 0, 2, 1]") != std::string::npos);
        CHECK(file.warnings[0].location.line == 0);
    }
    CHECK(easypb_schema::resolve_file(file, symbols, visible, true, error));
    CHECK(file.warnings.size() == 2);
}

void test_descriptor_only_nested_duplicates_name_own_path()
{
    // resolve_file is normally preceded by index_file (which rejects these
    // first); calling it directly exercises the defense-in-depth branch.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        for (int i = 0; i < 2; ++i) {
            DescriptorProto nested;
            nested.name = file.strings.save("N");
            nested.has_name = true;
            message.nested_type.push_back(nested);
        }
        file.file.message_type.push_back(message);
        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        std::set<const easypb_schema::SchemaFile*> visible;
        visible.insert(&file);
        CHECK(!easypb_schema::resolve_file(file, symbols, visible, true, error));
        CHECK(error.message.find("duplicate nested type name") != std::string::npos);
        CHECK(error.message.find("[4, 0, 3, 1]") != std::string::npos);
    }
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        for (int i = 0; i < 2; ++i) {
            EnumDescriptorProto enumeration;
            enumeration.name = file.strings.save("E");
            enumeration.has_name = true;
            message.enum_type.push_back(enumeration);
        }
        file.file.message_type.push_back(message);
        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        std::set<const easypb_schema::SchemaFile*> visible;
        visible.insert(&file);
        CHECK(!easypb_schema::resolve_file(file, symbols, visible, true, error));
        CHECK(error.message.find("duplicate nested type name") != std::string::npos);
        CHECK(error.message.find("[4, 0, 4, 1]") != std::string::npos);
    }
}

easypb_schema::DescriptorPath add_message(easypb_schema::SchemaFile& file,
                                          const char* name)
{
    DescriptorProto message;
    message.name = file.strings.save(name);
    message.has_name = true;
    file.file.message_type.push_back(message);
    return easypb_schema::file_message_path(
        static_cast<int>(file.file.message_type.size()) - 1);
}

void set_package(easypb_schema::SchemaFile& file, const char* package)
{
    file.file.package = file.strings.save(package);
    file.file.has_package = true;
}

void test_validate_deferred_file_skips_named_kinds()
{
    // A named packed message field and a bad named default pass: linking
    // may shadow the local match with an imported enum.
    {
        easypb_schema::SchemaFile file;
        set_package(file, "a.b");
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        FieldDescriptorProto packed = make_named_field(file, "items", 1, "b.E", true);
        packed.options.packed = true;
        packed.options.has_packed = true;
        packed.has_options = true;
        message.field.push_back(packed);
        FieldDescriptorProto choice = make_named_field(file, "choice", 2, "b.E", false);
        choice.default_value = file.strings.save("B");
        choice.has_default_value = true;
        message.field.push_back(choice);
        file.file.message_type.push_back(message);

        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::validate_deferred_file(file, error));
        CHECK(file.warnings.empty());
    }
    // Scalar rules still apply without linking.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        FieldDescriptorProto bad = make_scalar_field(file, "n", 1,
                                                     FieldDescriptorProto::TYPE_INT32);
        bad.default_value = file.strings.save("not-a-number");
        bad.has_default_value = true;
        message.field.push_back(bad);
        file.file.message_type.push_back(message);

        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::validate_deferred_file(file, error));
        CHECK(error.message.find("signed 32-bit") != std::string::npos);
    }
    // Packed on a non-repeated field fails without any type lookup.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        FieldDescriptorProto optional_packed =
            make_named_field(file, "s", 1, "State", false);
        optional_packed.options.packed = true;
        optional_packed.options.has_packed = true;
        optional_packed.has_options = true;
        message.field.push_back(optional_packed);
        file.file.message_type.push_back(message);

        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::validate_deferred_file(file, error));
        CHECK(error.message.find("packed option is valid only") != std::string::npos);
    }
    // Duplicate numbers stay immediate.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        message.field.push_back(
            make_scalar_field(file, "x", 1, FieldDescriptorProto::TYPE_INT32));
        message.field.push_back(
            make_scalar_field(file, "y", 1, FieldDescriptorProto::TYPE_INT32));
        file.file.message_type.push_back(message);

        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::validate_deferred_file(file, error));
        CHECK(error.message.find("duplicate field number") != std::string::npos);
    }
}

void test_validate_deferred_file_covers_nested_messages()
{
    // The link_types=false phase must reach nested messages too: with the
    // flag dropped at recursion, nested named fields would warn instead of
    // staying silently unresolved.
    easypb_schema::SchemaFile file;
    DescriptorProto message;
    message.name = file.strings.save("Outer");
    message.has_name = true;
    DescriptorProto nested;
    nested.name = file.strings.save("Inner");
    nested.has_name = true;
    FieldDescriptorProto field = make_named_field(file, "f", 1, "Foo", false);
    field.default_value = file.strings.save("BAR");
    field.has_default_value = true;
    nested.field.push_back(field);
    message.nested_type.push_back(nested);
    file.file.message_type.push_back(message);

    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::validate_deferred_file(file, error));
    CHECK(file.warnings.empty());
}

easypb_schema::DescriptorPath package_declaration_path()
{
    easypb_schema::DescriptorPath path;
    path.push_back(2);
    return path;
}

void test_package_scopes_merge_and_conflict()
{
    // Compatible packages merge across files in any order.
    {
        easypb_schema::SchemaFile first;
        set_package(first, "p");
        add_message(first, "A");
        easypb_schema::SchemaFile second;
        set_package(second, "p");
        add_message(second, "B");
        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::index_file(first, symbols, error));
        CHECK(easypb_schema::index_file(second, symbols, error));
        CHECK(symbols.symbols.find(".p.A") != symbols.symbols.end());
        CHECK(symbols.symbols.find(".p.B") != symbols.symbols.end());
    }
    // A package and a same-named message conflict in both orders, each
    // naming its own redeclaration position.
    for (int order = 0; order < 2; ++order) {
        easypb_schema::SchemaFile packaged;
        set_package(packaged, "p");
        add_message(packaged, "Value");
        declare(packaged, package_declaration_path(), 3);
        easypb_schema::SchemaFile global;
        add_message(global, "p");
        declare(global, easypb_schema::file_message_path(0), 2);
        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        if (order == 0) {
            CHECK(easypb_schema::index_file(packaged, symbols, error));
            CHECK(!easypb_schema::index_file(global, symbols, error));
            CHECK(error.location.line == 2);
        } else {
            CHECK(easypb_schema::index_file(global, symbols, error));
            CHECK(!easypb_schema::index_file(packaged, symbols, error));
            CHECK(error.location.line == 3);
        }
        CHECK(error.message.find("duplicate package name .p") != std::string::npos);
    }
    // Intermediate components count: package a.b conflicts with message a.
    {
        easypb_schema::SchemaFile packaged;
        set_package(packaged, "a.b");
        add_message(packaged, "Q");
        easypb_schema::SchemaFile global;
        add_message(global, "a");
        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::index_file(packaged, symbols, error));
        CHECK(!easypb_schema::index_file(global, symbols, error));
        CHECK(error.message.find("duplicate package name .a") != std::string::npos);
    }
    // A message sharing its package's spelling is fine within one file:
    // top-level declarations live under the package (".p.p"), so they can
    // never collide with the package node itself (".p").
    {
        easypb_schema::SchemaFile file;
        set_package(file, "p");
        add_message(file, "p");
        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::index_file(file, symbols, error));
        CHECK(symbols.symbols.find(".p.p") != symbols.symbols.end());
    }
}

void test_cross_file_type_symbol_text_is_order_independent()
{
    // Enum value .p.FOO (kind 0) versus message .p.FOO: either indexing
    // order must report the same text.
    for (int order = 0; order < 2; ++order) {
        easypb_schema::SchemaFile with_enum;
        set_package(with_enum, "p");
        EnumDescriptorProto enumeration;
        enumeration.name = with_enum.strings.save("E");
        enumeration.has_name = true;
        EnumValueDescriptorProto value;
        value.name = with_enum.strings.save("FOO");
        value.has_name = true;
        value.number = 0;
        value.has_number = true;
        enumeration.value.push_back(value);
        with_enum.file.enum_type.push_back(enumeration);

        easypb_schema::SchemaFile with_message;
        set_package(with_message, "p");
        add_message(with_message, "FOO");

        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        if (order == 0) {
            CHECK(easypb_schema::index_file(with_enum, symbols, error));
            CHECK(!easypb_schema::index_file(with_message, symbols, error));
        } else {
            CHECK(easypb_schema::index_file(with_message, symbols, error));
            CHECK(!easypb_schema::index_file(with_enum, symbols, error));
        }
        CHECK(error.message.find("duplicate type name .p.FOO") != std::string::npos);
    }
}

void test_strict_resolution_rejects_unresolved_types()
{
    easypb_schema::SchemaFile file;
    DescriptorProto message;
    message.name = file.strings.save("M");
    message.has_name = true;
    message.field.push_back(make_named_field(file, "value", 1, "missing.External", false));
    file.file.message_type.push_back(message);
    const easypb_schema::DescriptorPath field_path =
        easypb_schema::message_field_path(
            easypb_schema::file_message_path(0), 0);
    record_source(file, field_path, "missing.External", 7, false, 0, false, 0);

    easypb_schema::SymbolIndex symbols;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(file, symbols, error));
    std::set<const easypb_schema::SchemaFile*> visible;
    visible.insert(&file);
    CHECK(!easypb_schema::resolve_file(file, symbols, visible, false, error));
    CHECK(error.message.find("unresolved type") != std::string::npos);
    CHECK(error.location.line == 7);
}

void test_visibility_limits_resolution()
{
    easypb_schema::SchemaFile first;
    DescriptorProto message;
    message.name = first.strings.save("M");
    message.has_name = true;
    message.field.push_back(make_named_field(first, "e", 1, "E", false));
    first.file.message_type.push_back(message);
    const easypb_schema::DescriptorPath field_path =
        easypb_schema::message_field_path(
            easypb_schema::file_message_path(0), 0);
    record_source(first, field_path, "E", 3, false, 0, false, 0);

    easypb_schema::SchemaFile second;
    EnumDescriptorProto enumeration;
    enumeration.name = second.strings.save("E");
    enumeration.has_name = true;
    EnumValueDescriptorProto value;
    value.name = second.strings.save("ZERO");
    value.has_name = true;
    value.number = 0;
    value.has_number = true;
    enumeration.value.push_back(value);
    second.file.enum_type.push_back(enumeration);

    easypb_schema::SymbolIndex symbols;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(first, symbols, error));
    CHECK(easypb_schema::index_file(second, symbols, error));

    // The enum lives in an invisible file: no match.
    {
        std::set<const easypb_schema::SchemaFile*> visible;
        visible.insert(&first);
        CHECK(easypb_schema::resolve_file(first, symbols, visible, true, error));
        CHECK(first.warnings.size() == 1);
    }
    // A global symbol index does not grant global visibility: with both
    // files visible the same reference resolves.
    {
        first.warnings.clear();
        first.file.message_type[0].field[0].type =
            FieldDescriptorProto::TYPE_MESSAGE;
        std::set<const easypb_schema::SchemaFile*> visible;
        visible.insert(&first);
        visible.insert(&second);
        CHECK(easypb_schema::resolve_file(first, symbols, visible, true, error));
        CHECK(first.warnings.empty());
        const FieldDescriptorProto& field = first.file.message_type[0].field[0];
        CHECK(field.has_type && field.type == FieldDescriptorProto::TYPE_ENUM);
        CHECK(text(field.type_name) == ".E");
    }
}

void test_duplicate_declarations_are_rejected()
{
    // Duplicate top-level messages collide in the file scope.
    {
        easypb_schema::SchemaFile file;
        for (int i = 0; i < 2; ++i) {
            DescriptorProto message;
            message.name = file.strings.save("M");
            message.has_name = true;
            file.file.message_type.push_back(message);
        }
        declare(file, easypb_schema::file_message_path(0), 1);
        declare(file, easypb_schema::file_message_path(1), 5);
        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::index_file(file, symbols, error));
        // The code is part of the contract, not just the text (E1/A3): the
        // same defect across two files carries this code too, and
        // test_linker.cpp's test_conflicting_declaration_reports_one_code
        // asserts the pair (D-05).
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
        CHECK(error.message.find("name collision") != std::string::npos);
        CHECK(error.location.line == 5);
    }
    // Without recorded positions the error names the descriptor path and
    // uses an unknown location instead of fabricated coordinates.
    {
        easypb_schema::SchemaFile file;
        for (int i = 0; i < 2; ++i) {
            DescriptorProto message;
            message.name = file.strings.save("M");
            message.has_name = true;
            file.file.message_type.push_back(message);
        }
        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::index_file(file, symbols, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
        CHECK(error.message.find("[4, 1]") != std::string::npos);
        CHECK(error.location.line == 0);
    }
    // Duplicate field names collide in the message scope during indexing,
    // at the redeclaration position.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        message.field.push_back(
            make_scalar_field(file, "x", 1, FieldDescriptorProto::TYPE_INT32));
        message.field.push_back(
            make_scalar_field(file, "x", 2, FieldDescriptorProto::TYPE_INT32));
        file.file.message_type.push_back(message);
        const easypb_schema::DescriptorPath message_path =
            easypb_schema::file_message_path(0);
        declare(file, easypb_schema::message_field_path(message_path, 0), 3);
        declare(file, easypb_schema::message_field_path(message_path, 1), 4);

        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::index_file(file, symbols, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
        CHECK(error.message.find("name collision") != std::string::npos);
        CHECK(error.message.find("field x") != std::string::npos);
        CHECK(error.location.line == 4);
    }
    // Duplicate field numbers survive indexing and fail resolution.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        message.field.push_back(
            make_scalar_field(file, "x", 1, FieldDescriptorProto::TYPE_INT32));
        message.field.push_back(
            make_scalar_field(file, "y", 1, FieldDescriptorProto::TYPE_INT32));
        file.file.message_type.push_back(message);
        const easypb_schema::DescriptorPath message_path =
            easypb_schema::file_message_path(0);
        declare(file, easypb_schema::message_field_path(message_path, 0), 3);
        declare(file, easypb_schema::message_field_path(message_path, 1), 4);

        easypb_schema::SymbolIndex symbols;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::index_file(file, symbols, error));
        std::set<const easypb_schema::SchemaFile*> visible;
        visible.insert(&file);
        CHECK(!easypb_schema::resolve_file(file, symbols, visible, true, error));
        CHECK(error.message.find("duplicate field number") != std::string::npos);
        CHECK(error.location.line == 4);
    }
}

void test_message_defaults_are_rejected()
{
    easypb_schema::SchemaFile file;
    DescriptorProto message;
    message.name = file.strings.save("M");
    message.has_name = true;
    DescriptorProto nested;
    nested.name = file.strings.save("N");
    nested.has_name = true;
    message.nested_type.push_back(nested);
    FieldDescriptorProto field = make_named_field(file, "n", 1, "N", false);
    field.default_value = file.strings.save("{}");
    field.has_default_value = true;
    message.field.push_back(field);
    file.file.message_type.push_back(message);
    const easypb_schema::DescriptorPath field_path =
        easypb_schema::message_field_path(
            easypb_schema::file_message_path(0), 0);
    record_source(file, field_path, "N", 3, true, 4, false, 0);

    easypb_schema::SymbolIndex symbols;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(file, symbols, error));
    std::set<const easypb_schema::SchemaFile*> visible;
    visible.insert(&file);
    CHECK(!easypb_schema::resolve_file(file, symbols, visible, true, error));
    CHECK(error.message.find("message fields cannot have defaults") != std::string::npos);
    CHECK(error.location.line == 4);
}

// protoc's rule for the entry name a `map` field implies, and the reason the
// rule is one shared function: the parser synthesizes the entry with it and
// the linker later derives the same name with no parser linked at all
// (plan-4 amendments A22, A24). The expectations are measured, not inherited:
// libprotoc 30.1 generated exactly these entry names from exactly these field
// names, and plan-4 Appendix C records the same pairs as A1, A12, A14, A15.
// The last four rows are why there is no empty-name fallback - an all-underscore
// field must yield `Entry`, which the parser's old `Map` fallback got wrong -
// and why a message carrying `_` beside `__` is refused rather than named
// twice: both collapse onto `Entry`, exactly as they do in protoc (the pair's
// verdict is pinned in test_linker.cpp, ledger P-18).
void test_map_entry_name_follows_protoc()
{
    struct Case {
        const char* field_name;
        const char* entry_name;
    };
    const Case cases[] = {
        {"foo", "FooEntry"},
        {"foo_bar", "FooBarEntry"},
        {"a_b_c", "ABCEntry"},
        {"x__y", "XYEntry"},
        {"A_1b", "A1bEntry"},
        {"z9_9z", "Z99zEntry"},
        {"_lead", "LeadEntry"},
        {"Foo", "FooEntry"},
        {"_", "Entry"},
        {"__", "Entry"},
        {"___", "Entry"}
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(easypb_schema::map_entry_name(cases[i].field_name) ==
              cases[i].entry_name);
    }
}

// The shared integer parser behind every descriptor-origin numeric default
// (`parse_unsigned_integer` / `parse_signed_32`, both exported by
// schema_semantics.hpp). Muse's 0927 review reported it accepting `"0x"` as 0
// and the octal digits 8/9, and proposed exactly the `digit >= base` refusal
// the code has carried since before the reviewed range - so the finding was
// refuted by measurement (ledger P-18). These rows are that measurement kept
// as a test: the whole reported set, each base's own edges, and the signed
// wrapper, with the values asserted only where the parse succeeds (a failed
// parse leaves `result` partially written by design).
void test_integer_parsing_boundaries_follow_protoc()
{
    struct UnsignedCase {
        const char* text;
        bool ok;
        std::uint64_t value;
    };
    const UnsignedCase cases[] = {
        {"0", true, 0},
        {"00", true, 0},
        {"010", true, 8},            // octal, not decimal ten
        {"8", true, 8},
        {"0x0", true, 0},
        {"0x10", true, 16},
        {"536870911", true, 536870911ull},
        {"09", false, 0},            // 9 is not an octal digit
        {"08", false, 0},
        {"019", false, 0},
        {"0x", false, 0},            // the prefix alone is not a number
        {"0X", false, 0},
        {"0xG", false, 0},
        {"", false, 0},
        {"-1", false, 0}
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        std::uint64_t value = 0;
        const bool ok = easypb_schema::parse_unsigned_integer(cases[i].text,
                                                              value);
        CHECK(ok == cases[i].ok);
        if (ok) CHECK(value == cases[i].value);
    }

    struct SignedCase {
        const char* text;
        bool ok;
        std::int32_t value;
    };
    const SignedCase signed_cases[] = {
        {"0", true, 0},
        {"-0", true, 0},
        {"010", true, 8},
        {"0x10", true, 16},
        {"-0x10", true, -16},
        {"2147483647", true, 2147483647},
        {"-2147483648", true, (-2147483647 - 1)},
        {"2147483648", false, 0},    // one past INT32_MAX
        {"-2147483649", false, 0},   // one past INT32_MIN
        {"-09", false, 0},
        {"-0x", false, 0}
    };
    for (std::size_t i = 0;
         i < sizeof(signed_cases) / sizeof(signed_cases[0]); ++i) {
        std::int32_t value = 0;
        const bool ok = easypb_schema::parse_signed_32(signed_cases[i].text,
                                                       value);
        CHECK(ok == signed_cases[i].ok);
        if (ok) CHECK(value == signed_cases[i].value);
    }
}

// --- P1 rule split: direct calls -------------------------------------------
//
// resolve_message was split (P1) into five rule helpers, declared in
// schema_semantics_internal.hpp under easypb_schema::internal so each rule can
// be called on its own. These tests exercise one rule per case; a failure names
// the rule, and a shape the .proto parser cannot produce (a repeated field with
// a default, a oneof member with a label) can be probed directly.

// A target file declaring `enum E { A = 0; }`. An absent syntax is proto2, so
// passing "proto3" gives the cross-syntax counterpart.
easypb_schema::SchemaFile* make_enum_only_file(const char* syntax)
{
    easypb_schema::SchemaFile* file = new easypb_schema::SchemaFile();
    if (syntax != 0) {
        file->file.syntax = file->strings.save(syntax);
        file->file.has_syntax = true;
    }
    EnumDescriptorProto enumeration;
    enumeration.name = file->strings.save("E");
    enumeration.has_name = true;
    EnumValueDescriptorProto value;
    value.name = file->strings.save("A");
    value.has_name = true;
    value.number = 0;
    value.has_number = true;
    enumeration.value.push_back(value);
    file->file.enum_type.push_back(enumeration);
    declare(*file, easypb_schema::file_enum_path(0), 2);
    declare(*file,
            easypb_schema::enum_value_path(easypb_schema::file_enum_path(0), 0),
            3);
    return file;
}

void test_rule_shape_d55_repeated_default_is_refused()
{
    easypb_schema::SchemaFile file;
    DescriptorProto message;
    message.name = file.strings.save("M");
    message.has_name = true;
    FieldDescriptorProto repeated =
        make_scalar_field(file, "a", 1, FieldDescriptorProto::TYPE_INT32);
    repeated.label = FieldDescriptorProto::LABEL_REPEATED;
    repeated.default_value = file.strings.save("7");
    repeated.has_default_value = true;
    message.field.push_back(repeated);
    FieldDescriptorProto singular =
        make_scalar_field(file, "b", 2, FieldDescriptorProto::TYPE_INT32);
    singular.default_value = file.strings.save("7");
    singular.has_default_value = true;
    message.field.push_back(singular);
    file.file.message_type.push_back(message);

    const easypb_schema::DescriptorPath repeated_path =
        easypb_schema::message_field_path(easypb_schema::file_message_path(0), 0);
    const easypb_schema::DescriptorPath singular_path =
        easypb_schema::message_field_path(easypb_schema::file_message_path(0), 1);
    record_source(file, repeated_path, 0, 0, true, 4, false, 0);

    // D-55: a repeated field may not carry a default.
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::internal::validate_field_declared_shape(
        file, message.field[0], repeated_path, false, error));
    CHECK(error.message.find("repeated fields cannot have default values") !=
          std::string::npos);

    // Control: the same default on a singular proto2 field is legal, so the
    // refusal above is the label rule and not the presence of a default.
    CHECK(easypb_schema::internal::validate_field_declared_shape(
        file, message.field[1], singular_path, false, error));

    // F2/D-54: proto3 refuses an explicit default on any field, through the
    // same shape rule.
    easypb_schema::Diagnostic proto3_error;
    CHECK(!easypb_schema::internal::validate_field_declared_shape(
        file, message.field[1], singular_path, true, proto3_error));
    CHECK(proto3_error.message.find("proto3") != std::string::npos);
}

void test_rule_encoding_d51_packed_is_presence_and_value()
{
    easypb_schema::SchemaFile file;
    DescriptorProto message;
    message.name = file.strings.save("M");
    message.has_name = true;
    const easypb_schema::SymbolIndex symbols;
    const std::set<const easypb_schema::SchemaFile*> visible;

    // Repeated int32 with [packed = true] is legal.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "a", 1, FieldDescriptorProto::TYPE_INT32);
        field.label = FieldDescriptorProto::LABEL_REPEATED;
        field.options.packed = true;
        field.options.has_packed = true;
        field.has_options = true;
        const easypb_schema::DescriptorPath field_path =
            easypb_schema::message_field_path(
                easypb_schema::file_message_path(0), 0);
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::internal::validate_field_encoding(
            file, symbols, visible, "M", message, true, field, field_path, false,
            false, error));
    }
    // Packed on a singular field is refused without reading the kind.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "b", 2, FieldDescriptorProto::TYPE_INT32);
        field.options.packed = true;
        field.options.has_packed = true;
        field.has_options = true;
        const easypb_schema::DescriptorPath field_path =
            easypb_schema::message_field_path(
                easypb_schema::file_message_path(0), 1);
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::internal::validate_field_encoding(
            file, symbols, visible, "M", message, true, field, field_path, false,
            false, error));
        CHECK(error.message.find("packed option is valid only") !=
              std::string::npos);
    }
    // Packed on a repeated non-packable type is refused for a known kind.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "s", 3, FieldDescriptorProto::TYPE_STRING);
        field.label = FieldDescriptorProto::LABEL_REPEATED;
        field.options.packed = true;
        field.options.has_packed = true;
        field.has_options = true;
        const easypb_schema::DescriptorPath field_path =
            easypb_schema::message_field_path(
                easypb_schema::file_message_path(0), 2);
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::internal::validate_field_encoding(
            file, symbols, visible, "M", message, true, field, field_path, false,
            false, error));
        CHECK(error.message.find("packed option is valid only") !=
              std::string::npos);
    }
    // D-51: [packed = false] writes has_packed = true with packed = false and
    // is legal - the VALUE, not the presence, is the gate.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "c", 4, FieldDescriptorProto::TYPE_INT32);
        field.label = FieldDescriptorProto::LABEL_REPEATED;
        field.options.packed = false;
        field.options.has_packed = true;
        field.has_options = true;
        const easypb_schema::DescriptorPath field_path =
            easypb_schema::message_field_path(
                easypb_schema::file_message_path(0), 3);
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::internal::validate_field_encoding(
            file, symbols, visible, "M", message, true, field, field_path, false,
            false, error));
    }
}

void test_rule_encoding_d56_oneof_member_must_be_optional()
{
    easypb_schema::SchemaFile file;
    DescriptorProto message;
    message.name = file.strings.save("M");
    message.has_name = true;
    OneofDescriptorProto oneof;
    oneof.name = file.strings.save("choice");
    oneof.has_name = true;
    message.oneof_decl.push_back(oneof);
    const easypb_schema::SymbolIndex symbols;
    const std::set<const easypb_schema::SchemaFile*> visible;

    // D-56: a oneof member declared REPEATED is refused.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "a", 1, FieldDescriptorProto::TYPE_INT32);
        field.has_oneof_index = true;
        field.oneof_index = 0;
        field.label = FieldDescriptorProto::LABEL_REPEATED;
        const easypb_schema::DescriptorPath field_path =
            easypb_schema::message_field_path(
                easypb_schema::file_message_path(0), 0);
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::internal::validate_field_encoding(
            file, symbols, visible, "M", message, true, field, field_path, false,
            false, error));
        CHECK(error.message.find("must have label") != std::string::npos);
    }
    // An index past the declaration list names the message.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "b", 2, FieldDescriptorProto::TYPE_INT32);
        field.has_oneof_index = true;
        field.oneof_index = 5;
        const easypb_schema::DescriptorPath field_path =
            easypb_schema::message_field_path(
                easypb_schema::file_message_path(0), 1);
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::internal::validate_field_encoding(
            file, symbols, visible, "M", message, true, field, field_path, false,
            false, error));
        CHECK(error.message.find("invalid oneof_index in message M") !=
              std::string::npos);
    }
    // Control: an OPTIONAL member of the declared oneof passes.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "c", 3, FieldDescriptorProto::TYPE_INT32);
        field.has_oneof_index = true;
        field.oneof_index = 0;
        const easypb_schema::DescriptorPath field_path =
            easypb_schema::message_field_path(
                easypb_schema::file_message_path(0), 2);
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::internal::validate_field_encoding(
            file, symbols, visible, "M", message, true, field, field_path, false,
            false, error));
    }
}

void test_rule_oneofs_d28_unclaimed_declaration_is_refused()
{
    easypb_schema::SchemaFile file;
    DescriptorProto message;
    message.name = file.strings.save("M");
    message.has_name = true;
    OneofDescriptorProto oneof;
    oneof.name = file.strings.save("choice");
    oneof.has_name = true;
    message.oneof_decl.push_back(oneof);
    const easypb_schema::DescriptorPath message_path =
        easypb_schema::file_message_path(0);

    // D-28: a declaration no field claims is refused.
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::internal::validate_message_oneofs(
        file, message, "M", message_path, error));
    CHECK(error.message.find("has no fields") != std::string::npos);

    // Control: once a field claims it, the same declaration passes.
    FieldDescriptorProto field =
        make_scalar_field(file, "a", 1, FieldDescriptorProto::TYPE_INT32);
    field.has_oneof_index = true;
    field.oneof_index = 0;
    message.field.push_back(field);
    CHECK(easypb_schema::internal::validate_message_oneofs(
        file, message, "M", message_path, error));
}

void test_rule_type_a7_declared_kind_mismatch_is_refused()
{
    easypb_schema::SchemaFile* target = make_enum_only_file(0);
    easypb_schema::SymbolIndex symbols;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(*target, symbols, error));

    easypb_schema::SchemaFile requester;
    requester.from_descriptor_set = true;
    FieldDescriptorProto field = make_named_field(requester, "e", 1, ".E", false);
    // make_named_field declares TYPE_MESSAGE; the name resolves to an enum.
    const easypb_schema::DescriptorPath field_path =
        easypb_schema::message_field_path(easypb_schema::file_message_path(0), 0);
    std::set<const easypb_schema::SchemaFile*> visible;
    visible.insert(target);
    easypb_schema::internal::UnresolvedPolicy policy;
    bool unknown = false;
    bool external_reference = false;
    CHECK(!easypb_schema::internal::resolve_field_type(
        requester, symbols, visible, policy, "M", true, field, field_path, &unknown,
        &external_reference, error));
    CHECK(error.message.find("descriptor metadata is authoritative") !=
          std::string::npos);
    delete target;
}

void test_rule_type_proto3_field_cannot_use_proto2_enum()
{
    easypb_schema::SchemaFile* proto2_target = make_enum_only_file(0);
    easypb_schema::SymbolIndex symbols;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(*proto2_target, symbols, error));

    easypb_schema::SchemaFile requester;
    requester.file.syntax = requester.strings.save("proto3");
    requester.file.has_syntax = true;
    FieldDescriptorProto field =
        make_scalar_field(requester, "e", 1, FieldDescriptorProto::TYPE_ENUM);
    field.type_name = requester.strings.save(".E");
    field.has_type_name = true;
    const easypb_schema::DescriptorPath field_path =
        easypb_schema::message_field_path(easypb_schema::file_message_path(0), 0);
    std::set<const easypb_schema::SchemaFile*> visible;
    visible.insert(proto2_target);
    easypb_schema::internal::UnresolvedPolicy policy;
    bool unknown = false;
    bool external_reference = false;
    CHECK(!easypb_schema::internal::resolve_field_type(
        requester, symbols, visible, policy, "M", true, field, field_path, &unknown,
        &external_reference, error));
    CHECK(error.message.find("must not be used by a proto3 field") !=
          std::string::npos);
    delete proto2_target;

    // Control: the same reference resolves when both sides are proto3.
    easypb_schema::SchemaFile* proto3_target = make_enum_only_file("proto3");
    easypb_schema::SymbolIndex proto3_symbols;
    CHECK(easypb_schema::index_file(*proto3_target, proto3_symbols, error));
    FieldDescriptorProto resolved =
        make_scalar_field(requester, "e", 1, FieldDescriptorProto::TYPE_ENUM);
    resolved.type_name = requester.strings.save(".E");
    resolved.has_type_name = true;
    std::set<const easypb_schema::SchemaFile*> proto3_visible;
    proto3_visible.insert(proto3_target);
    bool resolved_unknown = false;
    bool resolved_external = false;
    CHECK(easypb_schema::internal::resolve_field_type(
        requester, proto3_symbols, proto3_visible, policy, "M", true, resolved,
        field_path, &resolved_unknown, &resolved_external, error));
    CHECK(resolved.type == FieldDescriptorProto::TYPE_ENUM);
    delete proto3_target;
}

void test_rule_nested_types_duplicates_and_recursion()
{
    // A nested type name is unique across BOTH the enum and the message list
    // (one shared name set), so a message colliding with an earlier enum is
    // refused.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        EnumDescriptorProto enumeration;
        enumeration.name = file.strings.save("E");
        enumeration.has_name = true;
        message.enum_type.push_back(enumeration);
        DescriptorProto nested;
        nested.name = file.strings.save("E");
        nested.has_name = true;
        message.nested_type.push_back(nested);
        file.file.message_type.push_back(message);
        const easypb_schema::DescriptorPath message_path =
            easypb_schema::file_message_path(0);

        easypb_schema::SymbolIndex symbols;
        const std::set<const easypb_schema::SchemaFile*> visible;
        easypb_schema::internal::UnresolvedPolicy policy;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::internal::validate_nested_types(
            file, symbols, visible, policy, true, message, "M", message_path,
            error));
        CHECK(error.message.find("duplicate nested type name in M") !=
              std::string::npos);
    }
    // Recursion: a nested message is handed to resolve_message, so an invalid
    // field inside it is refused here, without a resolve_file walk.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        DescriptorProto nested;
        nested.name = file.strings.save("N");
        nested.has_name = true;
        nested.field.push_back(
            make_scalar_field(file, "x", 0, FieldDescriptorProto::TYPE_INT32));
        message.nested_type.push_back(nested);
        file.file.message_type.push_back(message);
        const easypb_schema::DescriptorPath message_path =
            easypb_schema::file_message_path(0);

        easypb_schema::SymbolIndex symbols;
        const std::set<const easypb_schema::SchemaFile*> visible;
        easypb_schema::internal::UnresolvedPolicy policy;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::internal::validate_nested_types(
            file, symbols, visible, policy, true, message, "M", message_path,
            error));
        CHECK(error.message.find("outside the valid range") != std::string::npos);
    }
    // Control: distinct nested names and a valid nested message pass.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        EnumDescriptorProto enumeration;
        enumeration.name = file.strings.save("E");
        enumeration.has_name = true;
        message.enum_type.push_back(enumeration);
        DescriptorProto nested;
        nested.name = file.strings.save("N");
        nested.has_name = true;
        nested.field.push_back(
            make_scalar_field(file, "x", 1, FieldDescriptorProto::TYPE_INT32));
        message.nested_type.push_back(nested);
        file.file.message_type.push_back(message);
        const easypb_schema::DescriptorPath message_path =
            easypb_schema::file_message_path(0);

        easypb_schema::SymbolIndex symbols;
        const std::set<const easypb_schema::SchemaFile*> visible;
        easypb_schema::internal::UnresolvedPolicy policy;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::internal::validate_nested_types(
            file, symbols, visible, policy, true, message, "M", message_path,
            error));
    }
}

void add_enum_value(EnumDescriptorProto& enumeration,
                    easypb_schema::SchemaFile& file, const char* name,
                    int number)
{
    EnumValueDescriptorProto value;
    value.name = file.strings.save(name);
    value.has_name = true;
    value.number = number;
    value.has_number = true;
    enumeration.value.push_back(value);
}

void test_rule_default_validator_directly()
{
    // validate_default is the type-dependent validator behind rule 3, reached
    // through the internal seam; each case probes one scalar category.
    easypb_schema::SchemaFile file;
    const easypb_schema::SymbolIndex symbols;
    const std::set<const easypb_schema::SchemaFile*> visible;
    const easypb_schema::DescriptorPath field_path =
        easypb_schema::message_field_path(easypb_schema::file_message_path(0), 0);

    // A legal 32-bit default passes.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "a", 1, FieldDescriptorProto::TYPE_INT32);
        field.default_value = file.strings.save("7");
        field.has_default_value = true;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::internal::validate_default(
            file, symbols, visible, field, false, field_path, error));
    }
    // One past INT32_MAX is refused.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "a", 1, FieldDescriptorProto::TYPE_INT32);
        field.default_value = file.strings.save("2147483648");
        field.has_default_value = true;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::internal::validate_default(
            file, symbols, visible, field, false, field_path, error));
        CHECK(error.message.find("does not fit signed 32-bit field") !=
              std::string::npos);
    }
    // A bool default must be exactly true or false.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "a", 1, FieldDescriptorProto::TYPE_BOOL);
        field.default_value = file.strings.save("yes");
        field.has_default_value = true;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::internal::validate_default(
            file, symbols, visible, field, false, field_path, error));
        CHECK(error.message.find("bool default must be true or false") !=
              std::string::npos);
    }
    // A message field may not carry a default at all.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "a", 1, FieldDescriptorProto::TYPE_MESSAGE);
        field.default_value = file.strings.save("x");
        field.has_default_value = true;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::internal::validate_default(
            file, symbols, visible, field, false, field_path, error));
        CHECK(error.message.find("message fields cannot have defaults") !=
              std::string::npos);
    }
    // D-58: a bytes default is C-escaped; an unknown escape is refused.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "a", 1, FieldDescriptorProto::TYPE_BYTES);
        field.default_value = file.strings.save("\\q");
        field.has_default_value = true;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::internal::validate_default(
            file, symbols, visible, field, false, field_path, error));
        CHECK(error.message.find("invalid escaping in bytes default value") !=
              std::string::npos);
    }
    // An unknown named type defers with a warning instead of failing.
    {
        FieldDescriptorProto field = make_named_field(file, "a", 1, "State", false);
        field.default_value = file.strings.save("ACTIVE");
        field.has_default_value = true;
        easypb_schema::Diagnostic error;
        const std::size_t before = file.warnings.size();
        CHECK(easypb_schema::internal::validate_default(
            file, symbols, visible, field, true, field_path, error));
        CHECK(file.warnings.size() == before + 1);
    }
    // A field with no default is a no-op, even for a message kind.
    {
        FieldDescriptorProto field =
            make_scalar_field(file, "a", 1, FieldDescriptorProto::TYPE_MESSAGE);
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::internal::validate_default(
            file, symbols, visible, field, false, field_path, error));
    }
}

void test_rule_enum_value_numbers_directly()
{
    // validate_enum_value_numbers is public; these cases call it directly.
    // An enum with no values is refused.
    {
        easypb_schema::SchemaFile file;
        EnumDescriptorProto enumeration;
        enumeration.name = file.strings.save("E");
        enumeration.has_name = true;
        file.file.enum_type.push_back(enumeration);
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::validate_enum_value_numbers(file, error));
        CHECK(error.message.find("contains no values") != std::string::npos);
    }
    // Two values may not share a number without allow_alias.
    {
        easypb_schema::SchemaFile file;
        EnumDescriptorProto enumeration;
        enumeration.name = file.strings.save("E");
        enumeration.has_name = true;
        add_enum_value(enumeration, file, "A", 1);
        add_enum_value(enumeration, file, "B", 1);
        file.file.enum_type.push_back(enumeration);
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::validate_enum_value_numbers(file, error));
        CHECK(error.message.find("repeats number 1") != std::string::npos);
    }
    // ... unless the enum declares option allow_alias = true.
    {
        easypb_schema::SchemaFile file;
        EnumDescriptorProto enumeration;
        enumeration.name = file.strings.save("E");
        enumeration.has_name = true;
        enumeration.options.has_allow_alias = true;
        enumeration.options.allow_alias = true;
        add_enum_value(enumeration, file, "A", 1);
        add_enum_value(enumeration, file, "B", 1);
        file.file.enum_type.push_back(enumeration);
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::validate_enum_value_numbers(file, error));
    }
    // Distinct numbers pass without the option.
    {
        easypb_schema::SchemaFile file;
        EnumDescriptorProto enumeration;
        enumeration.name = file.strings.save("E");
        enumeration.has_name = true;
        add_enum_value(enumeration, file, "A", 0);
        add_enum_value(enumeration, file, "B", 1);
        file.file.enum_type.push_back(enumeration);
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::validate_enum_value_numbers(file, error));
    }
}

void test_rule_symbol_names_directly()
{
    // validate_symbol_names is public; these cases call it directly.
    // An illegal character in an enum name is refused.
    {
        easypb_schema::SchemaFile file;
        EnumDescriptorProto enumeration;
        enumeration.name = file.strings.save("bad-name");
        enumeration.has_name = true;
        file.file.enum_type.push_back(enumeration);
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::validate_symbol_names(file, error));
        CHECK(error.message.find("is not a valid identifier") != std::string::npos);
    }
    // An unnamed message is refused.
    {
        easypb_schema::SchemaFile file;
        DescriptorProto message;
        file.file.message_type.push_back(message);
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::validate_symbol_names(file, error));
        CHECK(error.message.find("has no name") != std::string::npos);
    }
    // Well-formed declarations pass.
    {
        easypb_schema::SchemaFile file;
        EnumDescriptorProto enumeration;
        enumeration.name = file.strings.save("E");
        enumeration.has_name = true;
        add_enum_value(enumeration, file, "A", 0);
        file.file.enum_type.push_back(enumeration);
        DescriptorProto message;
        message.name = file.strings.save("M");
        message.has_name = true;
        file.file.message_type.push_back(message);
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::validate_symbol_names(file, error));
    }
}

} // namespace

int main()
{
    test_index_and_local_resolution();
    test_invalid_local_enum_default_fails_at_default_location();
    test_packed_rules_need_a_known_kind();
    test_repeated_resolution_warns_once_per_field();
    test_descriptor_only_warnings_stay_per_field();
    test_descriptor_only_nested_duplicates_name_own_path();
    test_validate_deferred_file_skips_named_kinds();
    test_validate_deferred_file_covers_nested_messages();
    test_package_scopes_merge_and_conflict();
    test_strict_resolution_rejects_unresolved_types();
    test_visibility_limits_resolution();
    test_duplicate_declarations_are_rejected();
    test_cross_file_service_collision_names_symbol();
    test_cross_file_type_symbol_text_is_order_independent();
    test_message_defaults_are_rejected();
    test_map_entry_name_follows_protoc();
    test_integer_parsing_boundaries_follow_protoc();
    test_rule_shape_d55_repeated_default_is_refused();
    test_rule_encoding_d51_packed_is_presence_and_value();
    test_rule_encoding_d56_oneof_member_must_be_optional();
    test_rule_oneofs_d28_unclaimed_declaration_is_refused();
    test_rule_type_a7_declared_kind_mismatch_is_refused();
    test_rule_type_proto3_field_cannot_use_proto2_enum();
    test_rule_nested_types_duplicates_and_recursion();
    test_rule_default_validator_directly();
    test_rule_enum_value_numbers_directly();
    test_rule_symbol_names_directly();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all schema semantics tests passed\n";
    return EXIT_SUCCESS;
}
