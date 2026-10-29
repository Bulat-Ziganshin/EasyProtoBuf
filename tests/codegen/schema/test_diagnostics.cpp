// V-44 and V-50: direct unit cases for the diagnostics the schema lane owns.
//
// The descriptors are built by hand so the cases observe the layer that
// produces the sentence and the position, independent of the parser and of
// the CLI transcript:
//
//   - V-44: a cross-file message/service collision names the category of both
//     declarations instead of calling the service a type;
//   - V-50: the by-value cycle refusal blames the field that closes the walk,
//     not either message of the cycle.
//
// The message-dependency graph lives in easypb_codegen_core, so this target
// links it; the case still runs in every profile (the core is parser-free).

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "descriptor.pb.hpp"
#include "emitter_error.hpp"
#include "generation.hpp"
#include "schema.hpp"
#include "schema_linker.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition) {
        std::cerr << file << ':' << line << ": CHECK failed: " << expression
                  << '\n';
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __FILE__, __LINE__)

easypb_schema::SourceLocation at_line(int line)
{
    easypb_schema::SourceLocation location;
    location.offset = 0;
    location.line = static_cast<std::size_t>(line);
    location.column = 1;
    return location;
}

easypb_schema::SchemaFile& make_file(easypb_schema::SchemaSet& set,
                                     const char* name, const char* package)
{
    easypb_schema::SchemaFile& file = set.add_file(name);
    file.physical_name.clear();
    file.from_descriptor_set = true;
    file.file.name = file.strings.save(name);
    file.file.has_name = true;
    file.file.syntax = "proto2";
    file.file.has_syntax = true;
    if (package != 0) {
        file.file.package = file.strings.save(package);
        file.file.has_package = true;
    }
    return file;
}

DescriptorProto make_message(easypb_schema::SchemaFile& file,
                             const char* name)
{
    DescriptorProto message;
    message.name = file.strings.save(name);
    message.has_name = true;
    return message;
}

FieldDescriptorProto make_message_field(easypb_schema::SchemaFile& file,
                                        const char* name, int number,
                                        const char* type_name)
{
    FieldDescriptorProto field;
    field.name = file.strings.save(name);
    field.has_name = true;
    field.number = number;
    field.has_number = true;
    field.label = FieldDescriptorProto::LABEL_OPTIONAL;
    field.has_label = true;
    field.type = FieldDescriptorProto::TYPE_MESSAGE;
    field.has_type = true;
    field.type_name = file.strings.save(type_name);
    field.has_type_name = true;
    return field;
}

// V-44: `a.proto` declares message Svc, `b.proto` declares service Svc in the
// same package. The name-sorted index reaches a.proto first, so the service is
// the redeclaration the sentence is positioned at.
void test_service_collision_names_both_declarations()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& a = make_file(set, "a.proto", "p");
    a.file.message_type.push_back(make_message(a, "Svc"));
    a.declaration_locations[easypb_schema::file_message_path(0)] = at_line(2);

    easypb_schema::SchemaFile& b = make_file(set, "b.proto", "p");
    ServiceDescriptorProto service;
    service.name = b.strings.save("Svc");
    service.has_name = true;
    b.file.service.push_back(service);
    b.declaration_locations[easypb_schema::file_service_path(0)] = at_line(7);

    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::link_schema(
        set, easypb_schema::LINK_COMPLETE, linked, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
    CHECK(error.file == "b.proto");
    CHECK(error.location.line == 7);
    CHECK(error.message ==
          "duplicate name .p.Svc: a service in \"b.proto\"; already defined "
          "as a message in \"a.proto\"");
    // The old wording is not accepted as a prefix of the new sentence.
    CHECK(error.message.find("duplicate type name") == std::string::npos);
    CHECK(linked.files == 0);
}

// V-50: A -> B -> A. The DFS walks A's field b first and closes the cycle on
// B's field a, so that field is the declaration to blame. The self-recursive
// control closes on the only field the message declares.
void test_cycle_blames_the_closing_field()
{
    struct Case
    {
        const char* name;
        bool self_cycle;
    };
    const Case cases[] = {
        {"mutual", false},
        {"self", true},
    };

    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_file(set, "cyc.proto", "cyc");
        if (cases[i].self_cycle) {
            DescriptorProto a = make_message(file, "A");
            a.field.push_back(make_message_field(file, "a", 1, ".cyc.A"));
            file.file.message_type.push_back(a);
        } else {
            DescriptorProto a = make_message(file, "A");
            a.field.push_back(make_message_field(file, "b", 1, ".cyc.B"));
            file.file.message_type.push_back(a);
            DescriptorProto b = make_message(file, "B");
            b.field.push_back(make_message_field(file, "a", 1, ".cyc.A"));
            file.file.message_type.push_back(b);
        }
        set.add_target(file);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::link_schema(
            set, easypb_schema::LINK_COMPLETE, linked, error));

        // A is file_message_path(0); the closing field is the first field of
        // the message whose back edge closes the walk (A itself for `self`).
        const int closing_message = cases[i].self_cycle ? 0 : 1;
        const easypb_schema::DescriptorPath closing_field =
            easypb_schema::message_field_path(
                easypb_schema::file_message_path(closing_message), 0);

        easypb_codegen::GenerationOptions options;
        bool threw = false;
        easypb_schema::DescriptorPath blamed;
        std::string message;
        try {
            (void)easypb_codegen::generate_header(file, linked, options);
        } catch (const easypb_codegen::EmitterError& problem) {
            threw = true;
            blamed = problem.declaration_path();
            message = problem.what();
        } catch (const std::exception& problem) {
            std::cerr << cases[i].name << ": unexpected failure: "
                      << problem.what() << '\n';
            ++failures;
        }
        CHECK(threw);
        CHECK(blamed == closing_field);
        CHECK(blamed != easypb_schema::file_message_path(closing_message));
        if (cases[i].self_cycle) {
            CHECK(message ==
                  "recursive message value dependency: ::cyc::A -> ::cyc::A");
        } else {
            CHECK(message ==
                  "recursive message value dependency: "
                  "::cyc::A -> ::cyc::B -> ::cyc::A");
        }
    }
}

} // namespace

int main()
{
    test_service_collision_names_both_declarations();
    test_cycle_blames_the_closing_field();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
