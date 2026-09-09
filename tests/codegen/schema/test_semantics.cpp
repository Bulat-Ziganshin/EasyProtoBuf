// Tests for the parser-independent semantic module
// (codegen/schema_semantics.hpp/.cpp): symbol indexing and local/strict
// field resolution. Descriptors are built by hand so this test links only
// easypb_schema and keeps passing in descriptor-set-only builds.

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

#include "schema_semantics.hpp"

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

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all schema semantics tests passed\n";
    return EXIT_SUCCESS;
}
