// Tests for the linked-schema layer (codegen/schema_linker.*). This file
// links only easypb_schema -- no parser, no loader -- because it must keep
// passing in descriptor-set-only builds (EASYPB_CODEGEN_WITH_PROTO_PARSER=OFF).
// That single constraint decides which cases can live here: descriptor-only
// isolation, set growth, repeated runs, pool growth, and the Step-2 visibility
// sets do; anything that destroys a source buffer or asserts a real source
// location must not, and belongs in test_source_linker.cpp instead. Fixtures
// are in-memory descriptors with empty physical_name and unknown positions.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "schema.hpp"
#include "schema_linker.hpp"

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

// Adds a descriptor-origin file with the given logical name and dependency
// lists (the make_file shape test_graph.cpp uses). Indexes in public_indexes
// and weak_indexes address positions in deps. physical_name is left empty to
// prove the linker needs no filesystem identity.
easypb_schema::SchemaFile& make_file(easypb_schema::SchemaSet& set,
                                     const char* logical,
                                     const std::vector<std::string>& deps,
                                     const std::vector<std::int32_t>& public_indexes,
                                     const std::vector<std::int32_t>& weak_indexes)
{
    easypb_schema::SchemaFile& file = set.add_file(logical);
    file.physical_name.clear();
    for (std::size_t i = 0; i < deps.size(); ++i) {
        file.file.dependency.push_back(file.strings.save(deps[i]));
    }
    for (std::size_t i = 0; i < public_indexes.size(); ++i) {
        file.file.public_dependency.push_back(public_indexes[i]);
    }
    for (std::size_t i = 0; i < weak_indexes.size(); ++i) {
        file.file.weak_dependency.push_back(weak_indexes[i]);
    }
    return file;
}

void set_package(easypb_schema::SchemaFile& file, const char* package)
{
    file.file.package = file.strings.save(package);
    file.file.has_package = true;
}

void add_message(easypb_schema::SchemaFile& file, const char* name)
{
    DescriptorProto message;
    message.name = file.strings.save(name);
    message.has_name = true;
    file.file.message_type.push_back(message);
}

// A dependency-free file, optionally with a package: the shape the
// cross-file collision cases need, because they must reach the index without
// anything in the bind stage to complain about.
easypb_schema::SchemaFile& make_simple_file(easypb_schema::SchemaSet& set,
                                           const char* logical,
                                           const char* package)
{
    easypb_schema::SchemaFile& file = make_file(
        set, logical, std::vector<std::string>(), std::vector<std::int32_t>(),
        std::vector<std::int32_t>());
    if (package != 0) set_package(file, package);
    return file;
}

void add_enum(easypb_schema::SchemaFile& file, const char* enum_name,
              const char* value_name)
{
    EnumDescriptorProto enumeration;
    enumeration.name = file.strings.save(enum_name);
    enumeration.has_name = true;
    if (value_name != 0) {
        EnumValueDescriptorProto value;
        value.name = file.strings.save(value_name);
        value.has_name = true;
        value.number = 0;
        value.has_number = true;
        enumeration.value.push_back(value);
    }
    file.file.enum_type.push_back(enumeration);
}

void add_service(easypb_schema::SchemaFile& file, const char* name)
{
    ServiceDescriptorProto service;
    service.name = file.strings.save(name);
    service.has_name = true;
    file.file.service.push_back(service);
}

// Links a fixture whose two files must collide in the stage-3 index and
// CHECKs the duplicate-declaration contract (A3): the dedicated code, both
// logical file names in the message, the redeclaration recorded in
// error.file, a position or descriptor path for each declaration, and - E3 -
// nothing published. `redeclaring` is the file indexed second, which the
// name-sorted walk makes the alphabetically later one.
void check_duplicate_failure(easypb_schema::SchemaSet& set,
                             const char* expected_substring,
                             const char* redeclaring)
{
    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;

    CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                      linked, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
    CHECK(error.message.find(expected_substring) != std::string::npos);
    CHECK(error.message.find("\"a.proto\"") != std::string::npos);
    CHECK(error.message.find("\"b.proto\"") != std::string::npos);
    CHECK(error.message.find(" at [") != std::string::npos);
    CHECK(error.message != "link_schema: not implemented");
    CHECK(error.file == redeclaring);
    CHECK(linked.files == 0);
    CHECK(!linked.complete);
    CHECK(linked.symbols.symbols.empty());
    CHECK(linked.visible_files.empty());
}

// One message with one optional named field. The field is what keeps this
// fixture linkable from Step 5 on, when a failed link publishes nothing:
// type_name is the descriptor spelling of a type defined in another file.
void add_message_with_field(easypb_schema::SchemaFile& file, const char* message_name,
                            const char* field_name, int number, const char* type_name)
{
    DescriptorProto message;
    message.name = file.strings.save(message_name);
    message.has_name = true;

    FieldDescriptorProto field;
    field.name = file.strings.save(field_name);
    field.has_name = true;
    field.number = number;
    field.has_number = true;
    field.label = FieldDescriptorProto::LABEL_OPTIONAL;
    field.has_label = true;
    // Standalone placeholder: the resolution pass overwrites the kind.
    field.type = FieldDescriptorProto::TYPE_MESSAGE;
    field.has_type = true;
    field.type_name = file.strings.save(type_name);
    field.has_type_name = true;

    message.field.push_back(field);
    file.file.message_type.push_back(message);
}

// Step 4 helpers. Descriptor-origin fixtures have no FieldSource, so a field's
// raw reference is exactly what type_name carries - the same shape a decoded
// .pbs file has, which is why these tests can drive the shared resolver
// without a parser.

// Appends one field. A null type_name makes the field a scalar (no
// type_name), which is how the "a nearer non-type head" fixtures get a field
// whose name shadows nothing.
void add_field(easypb_schema::SchemaFile& file, DescriptorProto& message,
               const char* name, int number, int label, const char* type_name)
{
    FieldDescriptorProto field;
    field.name = file.strings.save(name);
    field.has_name = true;
    field.number = number;
    field.has_number = true;
    field.label = label;
    field.has_label = true;
    if (type_name == 0) {
        field.type = FieldDescriptorProto::TYPE_INT32;
        field.has_type = true;
    } else {
        field.type = FieldDescriptorProto::TYPE_MESSAGE;
        field.has_type = true;
        field.type_name = file.strings.save(type_name);
        field.has_type_name = true;
    }
    message.field.push_back(field);
}

void add_nested(easypb_schema::SchemaFile& file, DescriptorProto& parent,
                const char* name)
{
    DescriptorProto nested;
    nested.name = file.strings.save(name);
    nested.has_name = true;
    parent.nested_type.push_back(nested);
}

// Appends the synthetic entry message a map field implies: protoc's marker,
// key field 1, value field 2. A null value_type_name makes the value a scalar.
void add_map_entry(easypb_schema::SchemaFile& file, DescriptorProto& message,
                   const char* entry_name, const char* value_type_name)
{
    DescriptorProto entry;
    entry.name = file.strings.save(entry_name);
    entry.has_name = true;
    entry.options.map_entry = true;
    entry.options.has_map_entry = true;
    entry.has_options = true;

    FieldDescriptorProto key;
    key.name = file.strings.save("key");
    key.has_name = true;
    key.number = 1;
    key.has_number = true;
    key.label = FieldDescriptorProto::LABEL_OPTIONAL;
    key.has_label = true;
    key.type = FieldDescriptorProto::TYPE_INT32;
    key.has_type = true;
    entry.field.push_back(key);

    FieldDescriptorProto value;
    value.name = file.strings.save("value");
    value.has_name = true;
    value.number = 2;
    value.has_number = true;
    value.label = FieldDescriptorProto::LABEL_OPTIONAL;
    value.has_label = true;
    if (value_type_name == 0) {
        value.type = FieldDescriptorProto::TYPE_STRING;
        value.has_type = true;
    } else {
        value.type = FieldDescriptorProto::TYPE_MESSAGE;
        value.has_type = true;
        value.type_name = file.strings.save(value_type_name);
        value.has_type_name = true;
    }
    entry.field.push_back(value);

    message.nested_type.push_back(entry);
}

// Appends the repeated message field a map<> declaration becomes.
void add_map_field(easypb_schema::SchemaFile& file, DescriptorProto& message,
                   const char* field_name, int number,
                   const char* entry_type_name)
{
    FieldDescriptorProto field;
    field.name = file.strings.save(field_name);
    field.has_name = true;
    field.number = number;
    field.has_number = true;
    field.label = FieldDescriptorProto::LABEL_REPEATED;
    field.has_label = true;
    field.type = FieldDescriptorProto::TYPE_MESSAGE;
    field.has_type = true;
    field.type_name = file.strings.save(entry_type_name);
    field.has_type_name = true;
    message.field.push_back(field);
}

// Retypes one field of one top-level message: the cheap way to build a case
// that differs from its fixture by a single reference. A successful resolution
// overwrites type_name with the absolute resolved name, so a fixture that
// resolves twice must put the raw reference back first.
void retype_field(easypb_schema::SchemaFile& file, int message_index,
                  int field_index, int label, const char* type_name)
{
    FieldDescriptorProto& field =
        file.file.message_type[message_index].field[field_index];
    field.label = label;
    field.has_label = true;
    field.type_name = file.strings.save(type_name);
    field.has_type_name = true;
}

// The same for a field of a nested message.
void retype_nested_field(easypb_schema::SchemaFile& file, int message_index,
                         int nested_index, int field_index,
                         const char* type_name)
{
    FieldDescriptorProto& field = file.file.message_type[message_index]
                                      .nested_type[nested_index]
                                      .field[field_index];
    field.type_name = file.strings.save(type_name);
    field.has_type_name = true;
}

// Runs complete linking first (stages 2-4 publish the index and the visibility
// sets) and then resolves ONE file through the shared resolve path. Step 5
// wires that call into link_schema itself; until then the Step-4 tests drive
// resolve_file directly, which is why a resolution failure is reported here
// while the link stays green.
bool resolve_after_link(easypb_schema::SchemaSet& set,
                        easypb_schema::SchemaFile& file, bool allow_unresolved,
                        easypb_schema::Diagnostic& error)
{
    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic link_error;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE, linked,
                                     link_error));
    const std::map<const easypb_schema::SchemaFile*,
                   std::set<const easypb_schema::SchemaFile*> >::const_iterator
        found = linked.visible_files.find(&file);
    CHECK(found != linked.visible_files.end());
    if (found == linked.visible_files.end()) return false;
    return easypb_schema::resolve_file(file, linked.symbols, found->second,
                                       allow_unresolved, error);
}

// The map-entry fixture: message D legally owns the map field map_field_name
// (and therefore the synthesized entry entry_name) and message R is a second
// message available as a requester that owns no entry. Everything is
// descriptor-origin, so the entry reference is absolute and no FieldSource
// exists - the corpus shape (Appendix D).
void make_map_fixture(easypb_schema::SchemaSet& set, const char* map_field_name,
                      const char* entry_name)
{
    easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
    file.from_descriptor_set = true;

    DescriptorProto d;
    d.name = file.strings.save("D");
    d.has_name = true;
    add_map_entry(file, d, entry_name, 0);
    add_map_field(file, d, map_field_name, 1,
                  (".pkg.D." + std::string(entry_name)).c_str());
    file.file.message_type.push_back(d);

    DescriptorProto r;
    r.name = file.strings.save("R");
    r.has_name = true;
    file.file.message_type.push_back(r);
}

// The same shape split across two files: defs.proto owns the entry, a.proto
// imports it and holds the requester. requester_package decides whether a
// relative compound name can reach the entry at all.
void make_imported_map_fixture(easypb_schema::SchemaSet& set,
                               const char* requester_package)
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;

    easypb_schema::SchemaFile& defs = make_file(set, "defs.proto", no_deps,
                                               no_indexes, no_indexes);
    defs.from_descriptor_set = true;
    set_package(defs, "pkg");
    DescriptorProto d;
    d.name = defs.strings.save("D");
    d.has_name = true;
    add_map_entry(defs, d, "FooEntry", 0);
    add_map_field(defs, d, "foo", 1, ".pkg.D.FooEntry");
    defs.file.message_type.push_back(d);

    easypb_schema::SchemaFile& root = make_file(
        set, "a.proto", std::vector<std::string>(1, "defs.proto"),
        no_indexes, no_indexes);
    root.from_descriptor_set = true;
    set_package(root, requester_package);
    DescriptorProto r;
    r.name = root.strings.save("R");
    r.has_name = true;
    root.file.message_type.push_back(r);
}

// The plan-4 Task 1 fixture, built as in-memory descriptors:
//
//   a.proto  imports b.proto (ordinary, public, or weak)
//            package app; message A { optional shared.Value v = 1; }
//   b.proto  imports c.proto (ordinary or public)
//            package shared; message Value {}
//   c.proto  package shared; message Other {}
//
// The field points at the type b defines -- the one type that resolves in
// every variant -- and c's symbol is never referenced. That is not cosmetics:
// the fixture must link successfully in all six cases, because from Step 5 on
// a failed link publishes nothing and the assertions on Visible(a) would
// disappear with it. The variant where a's field points at c's symbol is the
// negative case and lives in Step 4.3.
struct Chain
{
    easypb_schema::SchemaFile* a;
    easypb_schema::SchemaFile* b;
    easypb_schema::SchemaFile* c;
};

Chain make_chain_fixture(easypb_schema::SchemaSet& set, bool public_bc, bool weak_ab)
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;
    const std::vector<std::int32_t> index_zero(1, 0);

    easypb_schema::SchemaFile& a = make_file(
        set, "a.proto", std::vector<std::string>(1, "b.proto"),
        no_indexes, weak_ab ? index_zero : no_indexes);
    set_package(a, "app");
    add_message_with_field(a, "A", "v", 1, ".shared.Value");

    easypb_schema::SchemaFile& b = make_file(
        set, "b.proto", std::vector<std::string>(1, "c.proto"),
        public_bc ? index_zero : no_indexes, no_indexes);
    set_package(b, "shared");
    add_message(b, "Value");

    easypb_schema::SchemaFile& c = make_file(
        set, "c.proto", no_deps, no_indexes, no_indexes);
    set_package(c, "shared");
    add_message(c, "Other");

    Chain chain = { &a, &b, &c };
    return chain;
}

// a.proto imports b.proto and c.proto; both of those public-import d.proto.
struct Diamond
{
    easypb_schema::SchemaFile* a;
    easypb_schema::SchemaFile* b;
    easypb_schema::SchemaFile* c;
    easypb_schema::SchemaFile* d;
};

Diamond make_diamond_fixture(easypb_schema::SchemaSet& set)
{
    const std::vector<std::int32_t> no_indexes;
    const std::vector<std::int32_t> index_zero(1, 0);
    std::vector<std::string> a_deps;
    a_deps.push_back("b.proto");
    a_deps.push_back("c.proto");

    easypb_schema::SchemaFile& a = make_file(set, "a.proto", a_deps,
                                             no_indexes, no_indexes);
    set_package(a, "app");
    add_message_with_field(a, "A", "v", 1, ".shared.Value");

    easypb_schema::SchemaFile& b = make_file(
        set, "b.proto", std::vector<std::string>(1, "d.proto"),
        index_zero, no_indexes);
    set_package(b, "shared");
    add_message(b, "Value");

    easypb_schema::SchemaFile& c = make_file(
        set, "c.proto", std::vector<std::string>(1, "d.proto"),
        index_zero, no_indexes);
    set_package(c, "shared");
    add_message(c, "Other");

    easypb_schema::SchemaFile& d = make_file(
        set, "d.proto", std::vector<std::string>(), no_indexes, no_indexes);
    set_package(d, "shared");
    add_message(d, "Deep");

    Diamond diamond = { &a, &b, &c, &d };
    return diamond;
}

// a.proto imports b.proto and b.proto imports a.proto: the bind stage rejects
// the cycle, which is the failure this file uses to pin the publish-on-success
// rule (A1, E3).
void make_cycle_fixture(easypb_schema::SchemaSet& set)
{
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaFile& a = make_file(
        set, "a.proto", std::vector<std::string>(1, "b.proto"),
        no_indexes, no_indexes);
    add_message(a, "A");
    easypb_schema::SchemaFile& b = make_file(
        set, "b.proto", std::vector<std::string>(1, "a.proto"),
        no_indexes, no_indexes);
    add_message(b, "B");
}

// The logical names of a file's visible set, sorted. Names are collected and
// sorted because the set is pointer-keyed: std::set iterates it in pointer
// order, and no output may depend on that (A2). A missing entry means the
// fixture did not publish the view at all, which no assertion accepts.
std::string visible_names(const easypb_schema::LinkedSchema& linked,
                          const easypb_schema::SchemaFile& file)
{
    const std::map<const easypb_schema::SchemaFile*,
                   std::set<const easypb_schema::SchemaFile*> >::const_iterator found =
        linked.visible_files.find(&file);
    if (found == linked.visible_files.end()) return "<not computed>";

    std::vector<std::string> names;
    for (std::set<const easypb_schema::SchemaFile*>::const_iterator it =
             found->second.begin();
         it != found->second.end(); ++it) {
        names.push_back(text((*it)->file.name));
    }
    std::sort(names.begin(), names.end());

    std::string joined;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!joined.empty()) joined += ',';
        joined += names[i];
    }
    return joined;
}

// Links a fixture in complete mode and CHECKs that the staged publish of Step
// 2.2 produced a view. The contract details live in one place,
// test_link_stub_publishes_stages_2_and_4; the visibility tests below only
// need the published view.
easypb_schema::LinkedSchema link_ok(easypb_schema::SchemaSet& set)
{
    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                     linked, error));
    return linked;
}

// Step 2.2's staged contract, extended by Step 3.1: stages 2, 3 and 4 ran and
// the view is published, but it is explicitly incomplete because stage 5 is
// not implemented. Step 5.1 replaces its success half with real linking.
void test_link_stub_publishes_stages_2_3_and_4()
{
    // Success path: the bind stage, the shared symbol index and the
    // visibility sets all ran, and the result says so.
    easypb_schema::SchemaSet set;
    const Chain chain = make_chain_fixture(set, false, false);
    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;

    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                     linked, error));
    CHECK(linked.files == &set);
    CHECK(!linked.complete);
    // Stage 3: one index over every file, in the walk order, with the type
    // names both files define and the field a references.
    CHECK(linked.symbols.symbols.count(".app.A") == 1);
    CHECK(linked.symbols.symbols.count(".app.A.v") == 1);
    CHECK(linked.symbols.symbols.count(".shared.Value") == 1);
    CHECK(linked.symbols.symbols.count(".shared.Other") == 1);
    CHECK(linked.symbols.symbols.count(".shared") == 1);   // package scope
    CHECK(linked.visible_files.size() == 3);   // every file, not just one
    CHECK(visible_names(linked, *chain.a) == "a.proto,b.proto");

    // Failure path (E3, A1): the bind stage rejects a cycle, and the result
    // stays in the invalid empty state -- never a partial publication.
    easypb_schema::SchemaSet cyclic;
    make_cycle_fixture(cyclic);
    linked = easypb_schema::LinkedSchema();
    error = easypb_schema::Diagnostic();

    CHECK(!easypb_schema::link_schema(cyclic, easypb_schema::LINK_COMPLETE,
                                      linked, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_IMPORT_CYCLE);
    CHECK(!error.message.empty());
    CHECK(error.message != "link_schema: not implemented");
    CHECK(linked.files == 0);
    CHECK(!linked.complete);
    CHECK(linked.symbols.symbols.empty());
    CHECK(linked.visible_files.empty());

    // A view published by an earlier successful call must not survive a later
    // failing one: the caller must never mistake a stale view for this link.
    easypb_schema::LinkedSchema reused;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                     reused, error));
    CHECK(reused.files == &set);
    CHECK(!easypb_schema::link_schema(cyclic, easypb_schema::LINK_COMPLETE,
                                      reused, error));
    CHECK(reused.files == 0);
    CHECK(!reused.complete);
    CHECK(reused.visible_files.empty());
}

// An ordinary b -> c edge makes only b visible in a: a normal dependency's own
// private dependencies do not leak to the importer (V3).
void test_visibility_ordinary_import_is_not_transitive()
{
    easypb_schema::SchemaSet set;
    const Chain chain = make_chain_fixture(set, false, false);
    const easypb_schema::LinkedSchema linked = link_ok(set);

    CHECK(visible_names(linked, *chain.a) == "a.proto,b.proto");
    CHECK(visible_names(linked, *chain.b) == "b.proto,c.proto");
    CHECK(visible_names(linked, *chain.c) == "c.proto");
}

// The same edge marked public re-exports c to a (V1).
void test_visibility_public_import_is_transitive()
{
    easypb_schema::SchemaSet set;
    const Chain chain = make_chain_fixture(set, true, false);
    const easypb_schema::LinkedSchema linked = link_ok(set);

    CHECK(visible_names(linked, *chain.a) == "a.proto,b.proto,c.proto");
    CHECK(visible_names(linked, *chain.b) == "b.proto,c.proto");
    CHECK(visible_names(linked, *chain.c) == "c.proto");
}

// Root status grants nothing: listing c as a second entry point of the link
// does not make it visible in a (V4).
void test_visibility_root_status_grants_nothing()
{
    easypb_schema::SchemaSet set;
    const Chain chain = make_chain_fixture(set, false, false);
    set.add_target(*chain.c);
    const easypb_schema::LinkedSchema linked = link_ok(set);

    CHECK(set.targets().size() == 1);
    CHECK(visible_names(linked, *chain.a) == "a.proto,b.proto");
    CHECK(visible_names(linked, *chain.c) == "c.proto");
}

// Sharing a package is not visibility: an unimported file in the same package
// stays out of Visible(a) (V5).
void test_visibility_same_package_without_import_is_invisible()
{
    easypb_schema::SchemaSet set;
    const Chain chain = make_chain_fixture(set, false, false);
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaFile& d = make_file(
        set, "d.proto", std::vector<std::string>(), no_indexes, no_indexes);
    set_package(d, "shared");
    add_message(d, "Extra");

    const easypb_schema::LinkedSchema linked = link_ok(set);

    CHECK(chain.c->file.has_package);
    CHECK(visible_names(linked, *chain.a) == "a.proto,b.proto");
    CHECK(visible_names(linked, *chain.a).find("d.proto") == std::string::npos);
    CHECK(visible_names(linked, d) == "d.proto");
}

// In complete mode a weak import is an ordinary required dependency, so the
// weak edge a -> b makes b visible exactly as a normal one does.
void test_visibility_weak_import_behaves_as_ordinary()
{
    easypb_schema::SchemaSet set;
    const Chain chain = make_chain_fixture(set, false, true);
    const easypb_schema::LinkedSchema linked = link_ok(set);

    CHECK(chain.a->file.weak_dependency.size() == 1);
    CHECK(visible_names(linked, *chain.a) == "a.proto,b.proto");
    CHECK(visible_names(linked, *chain.b) == "b.proto,c.proto");
}

// A diamond: a imports b and c, both public-import d. Every set is asserted in
// full, not by membership: a memo entry returned half-built would drop an edge
// from one of them, and "d appears once" could not see that (a std::set cannot
// hold a duplicate in the first place). Visible(b) is the observable form of
// PublicExports(b) here, because b's only import is the public one.
void test_visibility_diamond_public_exports_are_complete()
{
    easypb_schema::SchemaSet set;
    const Diamond diamond = make_diamond_fixture(set);
    const easypb_schema::LinkedSchema linked = link_ok(set);

    CHECK(visible_names(linked, *diamond.a) == "a.proto,b.proto,c.proto,d.proto");
    CHECK(visible_names(linked, *diamond.b) == "b.proto,d.proto");
    CHECK(visible_names(linked, *diamond.c) == "c.proto,d.proto");
    CHECK(visible_names(linked, *diamond.d) == "d.proto");
}

// Beyond the pack's required rows: public imports are re-exported
// recursively, not one level deep. With a -> b -> c -> d all public, every
// file below a is visible in a. A one-level PublicExports would satisfy every
// required row above (each of them reaches only one public hop), so this is
// the row that pins the recursion itself.
void test_visibility_public_chain_re_exports_recursively()
{
    const std::vector<std::int32_t> no_indexes;
    const std::vector<std::int32_t> index_zero(1, 0);

    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& a = make_file(
        set, "a.proto", std::vector<std::string>(1, "b.proto"),
        index_zero, no_indexes);
    set_package(a, "app");
    add_message_with_field(a, "A", "v", 1, ".shared.Deep");

    easypb_schema::SchemaFile& b = make_file(
        set, "b.proto", std::vector<std::string>(1, "c.proto"),
        index_zero, no_indexes);
    set_package(b, "shared");
    add_message(b, "Value");

    easypb_schema::SchemaFile& c = make_file(
        set, "c.proto", std::vector<std::string>(1, "d.proto"),
        index_zero, no_indexes);
    set_package(c, "shared");
    add_message(c, "Other");

    easypb_schema::SchemaFile& d = make_file(
        set, "d.proto", std::vector<std::string>(), no_indexes, no_indexes);
    set_package(d, "shared");
    add_message(d, "Deep");

    const easypb_schema::LinkedSchema linked = link_ok(set);

    CHECK(visible_names(linked, a) == "a.proto,b.proto,c.proto,d.proto");
    CHECK(visible_names(linked, b) == "b.proto,c.proto,d.proto");
    CHECK(visible_names(linked, c) == "c.proto,d.proto");
    CHECK(visible_names(linked, d) == "d.proto");
}

// Step 3.1/3.2: cross-file collisions. Each pair below is two
// dependency-free files, so nothing in the bind stage can be the reason the
// link fails, and the name-sorted walk indexes `a.proto` before `b.proto`.

void test_index_message_vs_message_across_files()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& a = make_simple_file(set, "a.proto", "dup");
    add_message(a, "Same");
    easypb_schema::SchemaFile& b = make_simple_file(set, "b.proto", "dup");
    add_message(b, "Same");

    check_duplicate_failure(set, "duplicate type name .dup.Same", "b.proto");
}

void test_index_message_vs_enum_across_files()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& a = make_simple_file(set, "a.proto", 0);
    add_message(a, "Mix");
    easypb_schema::SchemaFile& b = make_simple_file(set, "b.proto", 0);
    add_enum(b, "Mix", 0);

    check_duplicate_failure(set, "duplicate type name .Mix", "b.proto");
}

void test_index_type_vs_enum_value_across_files()
{
    // Enum values live in the containing scope, not under the enum type: a's
    // ZERO occupies ".ZERO", so b's message ZERO collides with it.
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& a = make_simple_file(set, "a.proto", 0);
    add_enum(a, "E", "ZERO");
    easypb_schema::SchemaFile& b = make_simple_file(set, "b.proto", 0);
    add_message(b, "ZERO");

    check_duplicate_failure(set, "duplicate type name .ZERO", "b.proto");
}

void test_index_type_vs_service_name_across_files()
{
    // A retained service name participates in collisions but can never be
    // used as a field type; the wording is decided by both sides, because at
    // least one of them is a type, so the text does not depend on which file
    // is indexed first.
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& a = make_simple_file(set, "a.proto", 0);
    add_service(a, "Svc");
    easypb_schema::SchemaFile& b = make_simple_file(set, "b.proto", 0);
    add_message(b, "Svc");

    check_duplicate_failure(set, "duplicate type name .Svc", "b.proto");
}

void test_index_package_vs_type_across_files()
{
    // Appendix C D2: `package foo.bar;` in one file, `message foo {}` in
    // another. Every package component is indexed, so the collision is on
    // ".foo" - the component, not the whole package name.
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& a = make_simple_file(set, "a.proto", "foo.bar");
    (void)a;
    easypb_schema::SchemaFile& b = make_simple_file(set, "b.proto", 0);
    add_message(b, "foo");

    check_duplicate_failure(set, "duplicate package name .foo", "b.proto");
}

void test_index_same_package_in_two_files_merges()
{
    // Package scopes merge across files and must not be re-coded as a
    // collision (Step 3.1).
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& a = make_simple_file(set, "a.proto", "a");
    add_message(a, "A");
    easypb_schema::SchemaFile& b = make_simple_file(set, "b.proto", "a");
    add_message(b, "B");

    const easypb_schema::LinkedSchema linked = link_ok(set);

    CHECK(linked.symbols.symbols.count(".a") == 1);      // one merged scope
    CHECK(linked.symbols.symbols.count(".a.A") == 1);
    CHECK(linked.symbols.symbols.count(".a.B") == 1);
    CHECK(linked.symbols.symbols.count(".a.A.B") == 0);
}

void test_index_message_and_field_names_live_in_different_scopes()
{
    // A message named like another message's field is not a collision: the
    // field's key is qualified by its containing scope.
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& a = make_simple_file(set, "a.proto", 0);
    add_message_with_field(a, "Holder", "value", 1, ".value");
    easypb_schema::SchemaFile& b = make_simple_file(set, "b.proto", 0);
    add_message(b, "value");

    const easypb_schema::LinkedSchema linked = link_ok(set);

    CHECK(linked.symbols.symbols.count(".Holder") == 1);
    CHECK(linked.symbols.symbols.count(".Holder.value") == 1);
    CHECK(linked.symbols.symbols.count(".value") == 1);
}

// Builds the collision pair used by the two tests below, in either insertion
// order: an enum value ZERO in a.proto and a message ZERO in b.proto.
void build_enum_value_collision_pair(easypb_schema::SchemaSet& set, bool reversed)
{
    const char* first = reversed ? "b.proto" : "a.proto";
    const char* second = reversed ? "a.proto" : "b.proto";
    if (reversed) {
        easypb_schema::SchemaFile& b = make_simple_file(set, first, 0);
        add_message(b, "ZERO");
        easypb_schema::SchemaFile& a = make_simple_file(set, second, 0);
        add_enum(a, "E", "ZERO");
        return;
    }
    easypb_schema::SchemaFile& a = make_simple_file(set, first, 0);
    add_enum(a, "E", "ZERO");
    easypb_schema::SchemaFile& b = make_simple_file(set, second, 0);
    add_message(b, "ZERO");
}

void test_index_collision_text_is_order_independent()
{
    // The first observable consequence of the name-sorted walk (A2): the same
    // two colliding files, added to the set in opposite orders, must produce
    // one diagnostic. The pair is chosen so that a wording decided by "which
    // side is the type" instead of by both sides would differ between the two
    // orders - the enum value expands to the same occupied name as the
    // message, but it is not a type.
    easypb_schema::Diagnostic forward;
    easypb_schema::Diagnostic backward;
    {
        easypb_schema::SchemaSet set;
        build_enum_value_collision_pair(set, false);
        easypb_schema::LinkedSchema linked;
        CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                          linked, forward));
    }
    {
        easypb_schema::SchemaSet set;
        build_enum_value_collision_pair(set, true);
        easypb_schema::LinkedSchema linked;
        CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                          linked, backward));
    }

    CHECK(forward.code == backward.code);
    CHECK(forward.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
    CHECK(forward.message == backward.message);
    CHECK(forward.file == backward.file);
    CHECK(forward.message.find("duplicate type name .ZERO") != std::string::npos);
}

void test_link_indexes_every_file_once()
{
    // Regression guard for "never cache the index" (Step 3.1, Appendix A):
    // index_file reports a file's own declarations as duplicates on a second
    // pass, so a cached index would make the second link fail while the first
    // one succeeded.
    easypb_schema::SchemaSet set;
    make_chain_fixture(set, false, false);

    const easypb_schema::LinkedSchema first = link_ok(set);
    const easypb_schema::LinkedSchema second = link_ok(set);

    CHECK(second.symbols.symbols.size() == first.symbols.symbols.size());
    CHECK(first.symbols.symbols.size() == 6);   // 2 package scopes, 3 types, 1 field
    CHECK(second.symbols.symbols.count(".shared.Value") == 1);
    CHECK(second.symbols.symbols.count(".app.A.v") == 1);
}

// Step 4.3, Task 1's negative fixture: a's field is retyped at c's symbol,
// which is defined behind b's ordinary edge. The definition exists in the
// compilation but not in Visible(a), so the answer is the invisible category
// naming c.proto - not the missing one that a name no file declares produces.
// The public edge re-exports c and the very same reference resolves
// (A9; oracle B3/B4/B5).
void test_resolution_separates_invisible_from_missing()
{
    {
        easypb_schema::SchemaSet set;
        const Chain chain = make_chain_fixture(set, false, false);
        easypb_schema::Diagnostic error;

        // Control: b's own symbol resolves through the ordinary edge and is
        // interned as an absolute name.
        CHECK(resolve_after_link(set, *chain.a, false, error));
        CHECK(text(chain.a->file.message_type[0].field[0].type_name) ==
              ".shared.Value");
        CHECK(chain.a->file.message_type[0].field[0].type ==
              FieldDescriptorProto::TYPE_MESSAGE);

        // c's symbol: known to the compilation, invisible from a.proto.
        retype_field(*chain.a, 0, 0, FieldDescriptorProto::LABEL_OPTIONAL,
                     "shared.Other");
        error = easypb_schema::Diagnostic();
        CHECK(!resolve_after_link(set, *chain.a, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);
        CHECK(error.file == "a.proto");
        CHECK(error.message.find(".shared.Other") != std::string::npos);
        CHECK(error.message.find("c.proto") != std::string::npos);
        CHECK(error.message.find("import") != std::string::npos);

        // A name no file declares: the same walk, the other category.
        retype_field(*chain.a, 0, 0, FieldDescriptorProto::LABEL_OPTIONAL,
                     "shared.Absent");
        error = easypb_schema::Diagnostic();
        CHECK(!resolve_after_link(set, *chain.a, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        CHECK(error.message.find("unresolved type shared.Absent") !=
              std::string::npos);
        CHECK(error.message.find(".app.A") != std::string::npos);
        // The committed package scope is named, so the failure does not read
        // like a plain typo and the two codes cannot collapse into each other.
        CHECK(error.message.find("\".shared.Absent\"") != std::string::npos);
    }
    {
        // The same fixture with a public b -> c edge (B4).
        easypb_schema::SchemaSet set;
        const Chain chain = make_chain_fixture(set, true, false);
        retype_field(*chain.a, 0, 0, FieldDescriptorProto::LABEL_OPTIONAL,
                     "shared.Other");
        easypb_schema::Diagnostic error;
        CHECK(resolve_after_link(set, *chain.a, false, error));
        CHECK(text(chain.a->file.message_type[0].field[0].type_name) ==
              ".shared.Other");
    }
}

// The plan's shadowing case with the measured expectation (A19, oracle
// B1/B2): an outer pkg.Choice.Item and a nearer empty pkg.Encloser.Choice. The
// relative reference commits to the nearer aggregate and fails naming the
// resolved compound path - neither a non-type nor a plain missing name - while
// the explicit absolute reference to the intended outer type passes.
void test_shadowing_commits_the_matched_aggregate()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
    {
        DescriptorProto choice;
        choice.name = file.strings.save("Choice");
        choice.has_name = true;
        add_nested(file, choice, "Item");
        file.file.message_type.push_back(choice);
    }
    {
        DescriptorProto encloser;
        encloser.name = file.strings.save("Encloser");
        encloser.has_name = true;
        add_nested(file, encloser, "Choice");
        DescriptorProto m;
        m.name = file.strings.save("M");
        m.has_name = true;
        add_field(file, m, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  "Choice.Item");
        encloser.nested_type.push_back(m);
        file.file.message_type.push_back(encloser);
    }

    easypb_schema::Diagnostic error;
    CHECK(!resolve_after_link(set, file, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
    CHECK(error.message.find("unresolved type Choice.Item") != std::string::npos);
    CHECK(error.message.find("\".pkg.Encloser.Choice.Item\"") !=
          std::string::npos);
    CHECK(error.message.find("resolved to") != std::string::npos);

    // B2: the absolute reference to the intended outer type still passes, and
    // it needs no outward fallback to get there.
    retype_nested_field(file, 1, 1, 0, ".pkg.Choice.Item");
    error = easypb_schema::Diagnostic();
    CHECK(resolve_after_link(set, file, false, error));
    CHECK(text(file.file.message_type[1].nested_type[1].field[0].type_name) ==
          ".pkg.Choice.Item");
    CHECK(file.file.message_type[1].nested_type[1].field[0].type ==
          FieldDescriptorProto::TYPE_MESSAGE);
}

// The two mirror cases that separate the corrected walk from a
// resolve-then-check one: a nearer candidate that is a FIELD (B8) or an enum
// VALUE (B13) never wins a component, so the outer aggregate still resolves;
// a whole name that IS a non-type is refused by code (B9).
void test_non_type_heads_do_not_shadow()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
        {
            DescriptorProto choice;
            choice.name = file.strings.save("Choice");
            choice.has_name = true;
            add_nested(file, choice, "Item");
            file.file.message_type.push_back(choice);
        }
        {
            DescriptorProto m;
            m.name = file.strings.save("M");
            m.has_name = true;
            add_field(file, m, "Choice", 1,
                      FieldDescriptorProto::LABEL_OPTIONAL, 0);
            add_field(file, m, "f", 2, FieldDescriptorProto::LABEL_OPTIONAL,
                      "Choice.Item");
            file.file.message_type.push_back(m);
        }
        easypb_schema::Diagnostic error;
        CHECK(resolve_after_link(set, file, false, error));
        CHECK(text(file.file.message_type[1].field[1].type_name) ==
              ".pkg.Choice.Item");
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
        {
            DescriptorProto choice;
            choice.name = file.strings.save("Choice");
            choice.has_name = true;
            add_nested(file, choice, "Item");
            file.file.message_type.push_back(choice);
        }
        {
            DescriptorProto n;
            n.name = file.strings.save("N");
            n.has_name = true;
            EnumDescriptorProto enumeration;
            enumeration.name = file.strings.save("E");
            enumeration.has_name = true;
            EnumValueDescriptorProto value;
            value.name = file.strings.save("Choice");
            value.has_name = true;
            value.number = 0;
            value.has_number = true;
            enumeration.value.push_back(value);
            n.enum_type.push_back(enumeration);
            add_field(file, n, "f", 2, FieldDescriptorProto::LABEL_OPTIONAL,
                      "Choice.Item");
            file.file.message_type.push_back(n);
        }
        easypb_schema::Diagnostic error;
        CHECK(resolve_after_link(set, file, false, error));
        CHECK(text(file.file.message_type[1].field[0].type_name) ==
              ".pkg.Choice.Item");
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
        DescriptorProto p;
        p.name = file.strings.save("P");
        p.has_name = true;
        add_field(file, p, "Thing", 1, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        add_field(file, p, "t", 2, FieldDescriptorProto::LABEL_OPTIONAL, "Thing");
        file.file.message_type.push_back(p);
        easypb_schema::Diagnostic error;
        CHECK(!resolve_after_link(set, file, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_NOT_A_TYPE);
        CHECK(error.message.find("\"Thing\"") != std::string::npos);
        CHECK(error.message.find("a field") != std::string::npos);
    }
}

// An invisible nearer candidate does not shadow either (B6/B10), while making
// its file a direct import makes the same head win and commit (B7/B11) - which
// is what tells a walk that filters candidates from one that resolves first
// and checks visibility second.
void test_invisible_candidates_do_not_shadow()
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;

    // B6/B7: the nearer candidate is a PACKAGE declared by a file outside
    // Visible(a).
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& root = make_file(
            set, "a.proto", std::vector<std::string>(1, "vis.proto"),
            no_indexes, no_indexes);
        set_package(root, "pkg");
        DescriptorProto m;
        m.name = root.strings.save("M");
        m.has_name = true;
        add_field(root, m, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL, "sub.X");
        root.file.message_type.push_back(m);

        easypb_schema::SchemaFile& visible = make_file(set, "vis.proto", no_deps,
                                                      no_indexes, no_indexes);
        set_package(visible, "sub");
        add_message(visible, "X");

        easypb_schema::SchemaFile& hidden = make_file(set, "hidden.proto", no_deps,
                                                      no_indexes, no_indexes);
        set_package(hidden, "pkg.sub");
        add_message(hidden, "Other");

        easypb_schema::Diagnostic error;
        CHECK(resolve_after_link(set, root, false, error));
        CHECK(text(root.file.message_type[0].field[0].type_name) == ".sub.X");

        root.file.dependency.push_back(root.strings.save("hidden.proto"));
        retype_field(root, 0, 0, FieldDescriptorProto::LABEL_OPTIONAL, "sub.X");
        error = easypb_schema::Diagnostic();
        CHECK(!resolve_after_link(set, root, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        CHECK(error.message.find("\".pkg.sub.X\"") != std::string::npos);
    }
    // B10/B11: the same pair with a nearer MESSAGE instead of a package.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& root = make_file(
            set, "a.proto", std::vector<std::string>(1, "vis.proto"),
            no_indexes, no_indexes);
        set_package(root, "pkg");
        DescriptorProto m;
        m.name = root.strings.save("M");
        m.has_name = true;
        add_field(root, m, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL, "sub.X");
        root.file.message_type.push_back(m);

        easypb_schema::SchemaFile& visible = make_file(set, "vis.proto", no_deps,
                                                      no_indexes, no_indexes);
        set_package(visible, "sub");
        add_message(visible, "X");

        easypb_schema::SchemaFile& hidden = make_file(set, "hidden.proto", no_deps,
                                                      no_indexes, no_indexes);
        set_package(hidden, "pkg");
        add_message(hidden, "sub");

        easypb_schema::Diagnostic error;
        CHECK(resolve_after_link(set, root, false, error));
        CHECK(text(root.file.message_type[0].field[0].type_name) == ".sub.X");

        root.file.dependency.push_back(root.strings.save("hidden.proto"));
        retype_field(root, 0, 0, FieldDescriptorProto::LABEL_OPTIONAL, "sub.X");
        error = easypb_schema::Diagnostic();
        CHECK(!resolve_after_link(set, root, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        CHECK(error.message.find("\".pkg.sub.X\"") != std::string::npos);
    }
}

// Package scopes merge across files, so the node the index keeps is owned by
// whichever file was indexed first - here the one file the requester cannot
// see. protoc decides package visibility by asking whether ANY visible file
// declares the package, and a plain owner test would reject this input, which
// is the failure class A9/R3 exists to prevent.
void test_merged_package_is_visible_when_any_declaring_file_is()
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaSet set;

    // Indexed first, invisible from r.proto, and it declares `package shared`.
    easypb_schema::SchemaFile& first = make_file(set, "a.proto", no_deps,
                                                 no_indexes, no_indexes);
    set_package(first, "shared");
    add_message(first, "Unrelated");

    easypb_schema::SchemaFile& root = make_file(
        set, "r.proto", std::vector<std::string>(1, "z.proto"), no_indexes,
        no_indexes);
    set_package(root, "app");
    DescriptorProto m;
    m.name = root.strings.save("M");
    m.has_name = true;
    add_field(root, m, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
              "shared.Value");
    root.file.message_type.push_back(m);

    easypb_schema::SchemaFile& second = make_file(set, "z.proto", no_deps,
                                                  no_indexes, no_indexes);
    set_package(second, "shared");
    add_message(second, "Value");

    easypb_schema::Diagnostic error;
    CHECK(resolve_after_link(set, root, false, error));
    CHECK(text(root.file.message_type[0].field[0].type_name) == ".shared.Value");

    // Oracle F4: the invisible first-indexed file declares only the SUB-package
    // `package shared.deep`, which still creates the parent package node with
    // that file as its owner. protoc accepts the same shape, and it settles the
    // question by comparing the package string, so what makes the head visible
    // is the visible file's exact `package shared` - not the sub-package.
    {
        easypb_schema::SchemaSet sub_set;
        easypb_schema::SchemaFile& deep = make_file(sub_set, "a.proto", no_deps,
                                                    no_indexes, no_indexes);
        set_package(deep, "shared.deep");
        add_message(deep, "Nope");

        easypb_schema::SchemaFile& sub_root = make_file(
            sub_set, "r.proto", std::vector<std::string>(1, "z.proto"),
            no_indexes, no_indexes);
        set_package(sub_root, "app");
        DescriptorProto request;
        request.name = sub_root.strings.save("M");
        request.has_name = true;
        add_field(sub_root, request, "f", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "shared.Value");
        sub_root.file.message_type.push_back(request);

        easypb_schema::SchemaFile& sub_vis = make_file(
            sub_set, "z.proto", no_deps, no_indexes, no_indexes);
        set_package(sub_vis, "shared");
        add_message(sub_vis, "Value");

        easypb_schema::Diagnostic sub_error;
        CHECK(resolve_after_link(sub_set, sub_root, false, sub_error));
        CHECK(text(sub_root.file.message_type[0].field[0].type_name) ==
              ".shared.Value");
    }
}

// Oracle F5: a visible file that declares only the SUB-package `shared.deep`
// does not make the parent package `shared` visible, so `shared.Value` fails.
// protoc's own package check compares the package string (and answers "is not
// defined" here); our compilation-wide index knows the `.shared` node exists,
// so the residual category is *invisible* naming the file that declares it.
void test_sub_package_does_not_make_the_parent_visible()
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaSet set;

    easypb_schema::SchemaFile& root = make_file(
        set, "a.proto", std::vector<std::string>(1, "deep.proto"), no_indexes,
        no_indexes);
    set_package(root, "app");
    DescriptorProto m;
    m.name = root.strings.save("M");
    m.has_name = true;
    add_field(root, m, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
              "shared.Value");
    root.file.message_type.push_back(m);

    easypb_schema::SchemaFile& deep = make_file(set, "deep.proto", no_deps,
                                                no_indexes, no_indexes);
    set_package(deep, "shared.deep");
    add_message(deep, "X");

    easypb_schema::Diagnostic error;
    CHECK(!resolve_after_link(set, root, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);
    CHECK(error.message.find("deep.proto") != std::string::npos);
}

// B14 vs B15: an enum value is not addressable through its enum type, so that
// reference fails under the matched enum with the compound path named, while
// the value named on its own IS found in its containing scope - and a found
// declaration that is not a type is a different category, not the same one.
void test_enum_value_member_paths()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
    add_enum(file, "SomeEnum", "VALUE");
    DescriptorProto m;
    m.name = file.strings.save("M");
    m.has_name = true;
    add_field(file, m, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
              "SomeEnum.VALUE");
    file.file.message_type.push_back(m);

    easypb_schema::Diagnostic error;
    CHECK(!resolve_after_link(set, file, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
    CHECK(error.message.find("SomeEnum.VALUE") != std::string::npos);
    CHECK(error.message.find("\".pkg.SomeEnum.VALUE\"") != std::string::npos);

    retype_field(file, 0, 0, FieldDescriptorProto::LABEL_OPTIONAL, "VALUE");
    error = easypb_schema::Diagnostic();
    CHECK(!resolve_after_link(set, file, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_NOT_A_TYPE);
    CHECK(error.message.find("\"VALUE\"") != std::string::npos);
    CHECK(error.message.find("an enum value") != std::string::npos);
}

// Self-recursion, mutual recursion inside one file, and a reference into a
// visible file: only IMPORT cycles are errors and stage 2 already rejected
// those, so resolution must leave all three shapes alone.
void test_legal_recursion_stays_legal()
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaSet set;

    easypb_schema::SchemaFile& root = make_file(
        set, "a.proto", std::vector<std::string>(1, "y.proto"), no_indexes,
        no_indexes);
    set_package(root, "pkg");
    {
        DescriptorProto node;
        node.name = root.strings.save("Node");
        node.has_name = true;
        add_field(root, node, "next", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  "Node");
        root.file.message_type.push_back(node);
    }
    {
        DescriptorProto a;
        a.name = root.strings.save("A");
        a.has_name = true;
        add_field(root, a, "b", 1, FieldDescriptorProto::LABEL_OPTIONAL, "B");
        root.file.message_type.push_back(a);

        DescriptorProto b;
        b.name = root.strings.save("B");
        b.has_name = true;
        add_field(root, b, "a", 1, FieldDescriptorProto::LABEL_OPTIONAL, "A");
        root.file.message_type.push_back(b);
    }
    {
        DescriptorProto x;
        x.name = root.strings.save("X");
        x.has_name = true;
        add_field(root, x, "y", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  ".q.Y");
        root.file.message_type.push_back(x);
    }

    easypb_schema::SchemaFile& other = make_file(set, "y.proto", no_deps,
                                                 no_indexes, no_indexes);
    set_package(other, "q");
    DescriptorProto y;
    y.name = other.strings.save("Y");
    y.has_name = true;
    add_field(other, y, "self", 1, FieldDescriptorProto::LABEL_OPTIONAL,
              ".q.Y");
    other.file.message_type.push_back(y);

    easypb_schema::Diagnostic error;
    CHECK(resolve_after_link(set, root, false, error));
    CHECK(text(root.file.message_type[0].field[0].type_name) == ".pkg.Node");
    CHECK(text(root.file.message_type[1].field[0].type_name) == ".pkg.B");
    CHECK(text(root.file.message_type[2].field[0].type_name) == ".pkg.A");
    CHECK(text(root.file.message_type[3].field[0].type_name) == ".q.Y");
    CHECK(resolve_after_link(set, other, false, error));
    CHECK(text(other.file.message_type[0].field[0].type_name) == ".q.Y");
}

// A1: a legal map field resolves to its synthesized entry in both spellings a
// descriptor can carry - the absolute reference of a decoded file and the
// relative one the parser writes.
void test_legal_map_fields_resolve_to_their_entry()
{
    {
        easypb_schema::SchemaSet set;
        make_map_fixture(set, "foo", "FooEntry");
        easypb_schema::SchemaFile& file = *set.find_file("a.proto");
        easypb_schema::Diagnostic error;
        CHECK(resolve_after_link(set, file, false, error));
        CHECK(text(file.file.message_type[0].field[0].type_name) ==
              ".pkg.D.FooEntry");
        CHECK(file.file.message_type[0].field[0].type ==
              FieldDescriptorProto::TYPE_MESSAGE);
    }
    {
        easypb_schema::SchemaSet set;
        make_map_fixture(set, "foo", "FooEntry");
        easypb_schema::SchemaFile& file = *set.find_file("a.proto");
        retype_field(file, 0, 0, FieldDescriptorProto::LABEL_REPEATED, "FooEntry");
        easypb_schema::Diagnostic error;
        CHECK(resolve_after_link(set, file, false, error));
        CHECK(text(file.file.message_type[0].field[0].type_name) ==
              ".pkg.D.FooEntry");
    }
}

// Adds one reference to the requester message of a fresh map fixture and
// CHECKs that resolution refuses it with the map-entry code (A22/A24).
// message_index 0 is the owning message D - the "own entry" shapes - and 1 is
// the requester R.
void check_map_entry_rejection(const char* map_field_name,
                               const char* entry_name, int message_index,
                               const char* field_name, int label,
                               const char* type_name)
{
    easypb_schema::SchemaSet set;
    make_map_fixture(set, map_field_name, entry_name);
    easypb_schema::SchemaFile& file = *set.find_file("a.proto");
    add_field(file, file.file.message_type[message_index], field_name, 2, label,
              type_name);
    easypb_schema::Diagnostic error;
    CHECK(!resolve_after_link(set, file, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_MAP_ENTRY_TYPE);
    CHECK(error.message.find("map entry") != std::string::npos);
    CHECK(error.message.find(entry_name) != std::string::npos);
}

// Every user-facing spelling of a synthetic entry fails, and each of these
// rows is a shape protoc rejects too (A2-A6, A10, A11, E3, E4, E5). The
// repeated rows are the ones that prove the field NAME is consulted: a
// structural test alone accepts them.
void test_foreign_map_entry_references_are_rejected()
{
    // A2 and A10: a field of the owning message itself, by simple name,
    // singular and repeated.
    check_map_entry_rejection("foo", "FooEntry", 0, "own",
                              FieldDescriptorProto::LABEL_OPTIONAL, "FooEntry");
    check_map_entry_rejection("foo", "FooEntry", 0, "e",
                              FieldDescriptorProto::LABEL_REPEATED, "FooEntry");
    // A11/E3: the same repeated field with the absolute reference a descriptor
    // carries, which is the descriptor-level shape E3 records.
    check_map_entry_rejection("foo", "FooEntry", 1, "e",
                              FieldDescriptorProto::LABEL_REPEATED,
                              ".pkg.D.FooEntry");
    // A3/A4: another message's entry, by compound name. A requester inside the
    // owning message would resolve the compound name to .pkg.D.D.FooEntry and
    // fail as missing under the matched message instead; both verdicts reject,
    // and protoc's own algorithm walks the same way.
    check_map_entry_rejection("foo", "FooEntry", 1, "e",
                              FieldDescriptorProto::LABEL_OPTIONAL,
                              "D.FooEntry");
    // The row that makes the NESTING half of the ownership rule load-bearing:
    // a repeated field of another message whose name does derive the entry's
    // name is still not that entry's owning map field. Without the nesting
    // test this reference is accepted (protoc rejects it).
    check_map_entry_rejection("foo", "FooEntry", 1, "foo",
                              FieldDescriptorProto::LABEL_REPEATED,
                              "D.FooEntry");
    // E5: a singular field whose name DOES derive the entry name - the
    // repeated label is part of the rule, not decoration.
    check_map_entry_rejection("foo_bar", "FooBarEntry", 0, "fooBar",
                              FieldDescriptorProto::LABEL_OPTIONAL,
                              "FooBarEntry");

    // E4: the entry was renamed off the derivation, so protoc no longer
    // recognizes the field as a map field at all and rejects it even though no
    // user field references the entry. A structural-only guard accepts this.
    {
        easypb_schema::SchemaSet set;
        make_map_fixture(set, "foo", "ItemEntry");
        easypb_schema::SchemaFile& file = *set.find_file("a.proto");
        easypb_schema::Diagnostic error;
        CHECK(!resolve_after_link(set, file, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MAP_ENTRY_TYPE);
        CHECK(error.message.find("ItemEntry") != std::string::npos);
    }

    // A5: an entry owned by another file, named absolutely.
    {
        easypb_schema::SchemaSet set;
        make_imported_map_fixture(set, "app");
        easypb_schema::SchemaFile& root = *set.find_file("a.proto");
        add_field(root, root.file.message_type[0], "e", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, ".pkg.D.FooEntry");
        easypb_schema::Diagnostic error;
        CHECK(!resolve_after_link(set, root, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MAP_ENTRY_TYPE);
    }
    // A6: the same entry by the relative compound name, which reaches it only
    // because requester and owner share a package.
    {
        easypb_schema::SchemaSet set;
        make_imported_map_fixture(set, "pkg");
        easypb_schema::SchemaFile& root = *set.find_file("a.proto");
        add_field(root, root.file.message_type[0], "e", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "D.FooEntry");
        easypb_schema::Diagnostic error;
        CHECK(!resolve_after_link(set, root, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MAP_ENTRY_TYPE);
    }
}

// A9: the guard runs AFTER visibility. An entry whose owning file is not
// imported is an ordinary invisible type naming that file, never the
// map-entry answer.
void test_map_entry_guard_runs_after_visibility()
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaSet set;

    easypb_schema::SchemaFile& hidden = make_file(set, "hidden.proto", no_deps,
                                                  no_indexes, no_indexes);
    hidden.from_descriptor_set = true;
    set_package(hidden, "pkg");
    DescriptorProto h;
    h.name = hidden.strings.save("H");
    h.has_name = true;
    add_map_entry(hidden, h, "FooEntry", 0);
    add_map_field(hidden, h, "foo", 1, ".pkg.H.FooEntry");
    hidden.file.message_type.push_back(h);

    easypb_schema::SchemaFile& root = make_simple_file(set, "a.proto", "app");
    root.from_descriptor_set = true;
    DescriptorProto r;
    r.name = root.strings.save("R");
    r.has_name = true;
    add_field(root, r, "e", 1, FieldDescriptorProto::LABEL_OPTIONAL,
              ".pkg.H.FooEntry");
    root.file.message_type.push_back(r);

    easypb_schema::Diagnostic error;
    CHECK(!resolve_after_link(set, root, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);
    CHECK(error.message.find("hidden.proto") != std::string::npos);
    CHECK(error.message.find("map entry") == std::string::npos);
}

// The synthesized entry's value field is resolved from its own reference, not
// through the map field's synthetic entry name: the other route would silently
// defeat the cross-syntax check Step 6.2 adds (the classification itself is
// Step 6's, so this test asserts the precondition only).
void test_map_entry_value_resolves_from_its_own_token()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
    file.from_descriptor_set = true;
    add_enum(file, "SomeEnum", "ZERO");
    DescriptorProto m;
    m.name = file.strings.save("M");
    m.has_name = true;
    add_map_entry(file, m, "FooEntry", ".pkg.SomeEnum");
    add_map_field(file, m, "foo", 1, ".pkg.M.FooEntry");
    file.file.message_type.push_back(m);

    easypb_schema::Diagnostic error;
    CHECK(resolve_after_link(set, file, false, error));
    CHECK(text(file.file.message_type[0].field[0].type_name) ==
          ".pkg.M.FooEntry");
    const FieldDescriptorProto& value =
        file.file.message_type[0].nested_type[0].field[1];
    CHECK(value.has_type && value.type == FieldDescriptorProto::TYPE_ENUM);
    CHECK(text(value.type_name) == ".pkg.SomeEnum");
}

} // namespace

int main()
{
    test_link_stub_publishes_stages_2_3_and_4();
    test_visibility_ordinary_import_is_not_transitive();
    test_visibility_public_import_is_transitive();
    test_visibility_root_status_grants_nothing();
    test_visibility_same_package_without_import_is_invisible();
    test_visibility_weak_import_behaves_as_ordinary();
    test_visibility_diamond_public_exports_are_complete();
    test_visibility_public_chain_re_exports_recursively();
    test_index_message_vs_message_across_files();
    test_index_message_vs_enum_across_files();
    test_index_type_vs_enum_value_across_files();
    test_index_type_vs_service_name_across_files();
    test_index_package_vs_type_across_files();
    test_index_same_package_in_two_files_merges();
    test_index_message_and_field_names_live_in_different_scopes();
    test_index_collision_text_is_order_independent();
    test_link_indexes_every_file_once();
    test_resolution_separates_invisible_from_missing();
    test_shadowing_commits_the_matched_aggregate();
    test_non_type_heads_do_not_shadow();
    test_invisible_candidates_do_not_shadow();
    test_merged_package_is_visible_when_any_declaring_file_is();
    test_sub_package_does_not_make_the_parent_visible();
    test_enum_value_member_paths();
    test_legal_recursion_stays_legal();
    test_legal_map_fields_resolve_to_their_entry();
    test_foreign_map_entry_references_are_rejected();
    test_map_entry_guard_runs_after_visibility();
    test_map_entry_value_resolves_from_its_own_token();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all schema linker tests passed\n";
    return EXIT_SUCCESS;
}
