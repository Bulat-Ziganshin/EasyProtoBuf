// Tests for the linked-schema layer (codegen/schema_linker.*). This file
// links only easypb_schema -- no parser, no loader -- because it must keep
// passing in descriptor-set-only builds (EASYPB_CODEGEN_WITH_PROTO_PARSER=OFF).
// That single constraint decides which cases can live here: descriptor-only
// isolation, set growth, repeated runs, pool growth, and the Step-2 visibility
// sets do; anything that destroys a source buffer or asserts a real source
// location must not, and belongs in test_source_linker.cpp instead. Fixtures
// are in-memory descriptors with empty physical_name and unknown positions.
// The Step 7.4 corpus checkpoint decodes the tracked `.pbs` files through the
// directory argument tests/CMakeLists.txt passes, and still links no parser.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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

// The (count, value) constructor of a vector of integers is ambiguous on the
// earliest supported Clang: with both arguments `int`, the iterator-pair
// template deduces `InputIt = int` and is preferred over the non-template
// overload, so libstdc++ 4.8 rejects the instantiation. Spelling the count as
// a size_type makes the template non-viable and keeps this constructor.
std::vector<std::int32_t> repeated_indexes(std::size_t count,
                                           std::int32_t value)
{
    return std::vector<std::int32_t>(count, value);
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
    CHECK(!linked.local_only);
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
// value_kind is the kind a descriptor-origin value member DECLARES: the
// declared-kind check (A7) compares it with the definition the name resolves
// to, so an enum-valued entry must pass TYPE_ENUM, not the message default.
void add_map_entry(easypb_schema::SchemaFile& file, DescriptorProto& message,
                   const char* entry_name, const char* value_type_name,
                   int value_kind = FieldDescriptorProto::TYPE_MESSAGE)
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
        value.type = value_kind;
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

// The verdict of a complete link: true when every file resolved against its
// own visible set and a complete view was published, false when the first
// failure (in name-sorted file order) was reported in error. Step 5 wired
// stage 5 into link_schema, so this IS the resolution verdict the Step-4
// tests want: a helper that linked a stub-published index and then resolved
// one file by hand could not run any more, because a resolution failure is now
// the link's own failure and nothing is published for it.
bool link_verdict(easypb_schema::SchemaSet& set,
                  easypb_schema::Diagnostic& error)
{
    easypb_schema::LinkedSchema linked;
    const bool linked_ok = easypb_schema::link_schema(
        set, easypb_schema::LINK_COMPLETE, linked, error);
    if (linked_ok) {
        CHECK(linked.files == &set);
        CHECK(linked.local_only);
    } else {
        // Publish-on-failure (A1, E3): nothing readable survives a failure.
        CHECK(linked.files == 0);
        CHECK(!linked.local_only);
    }
    return linked_ok;
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
    const std::vector<std::int32_t> index_zero = repeated_indexes(1, 0);

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
    const std::vector<std::int32_t> index_zero = repeated_indexes(1, 0);
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

// Links a fixture in complete mode and CHECKs that a view was published. The
// contract details live in one place, test_complete_link_publishes_after_every
// _stage; the visibility tests below only need the published view.
easypb_schema::LinkedSchema link_ok(easypb_schema::SchemaSet& set)
{
    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                     linked, error));
    return linked;
}

// Step 5.1's success contract, extended across the earlier steps: the bind
// stage, the shared symbol index, the visibility sets and the resolution pass
// all ran, the staged publish of Step 2.2 is gone, and the result is a
// COMPLETE link. The failure paths below are unchanged - a failing stage still
// publishes nothing at all.
void test_complete_link_publishes_after_every_stage()
{
    // Success path.
    easypb_schema::SchemaSet set;
    const Chain chain = make_chain_fixture(set, false, false);
    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;

    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                     linked, error));
    CHECK(linked.files == &set);
    CHECK(linked.local_only);
    // Stage 5: the field resolved against a's visible set and was rewritten to
    // the absolute name the index holds.
    CHECK(text(chain.a->file.message_type[0].field[0].type_name) ==
          ".shared.Value");
    CHECK(chain.a->file.message_type[0].field[0].type ==
          FieldDescriptorProto::TYPE_MESSAGE);
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
    CHECK(!linked.local_only);
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
    CHECK(!reused.local_only);
    CHECK(reused.visible_files.empty());

    // The same rule for the resolution stage: the failure below is reported by
    // stage 5, and the reused result - which holds the SUCCESSFUL view of the
    // chain above - must end up invalid-empty rather than half-updated.
    easypb_schema::SchemaSet unresolved;
    {
        easypb_schema::SchemaFile& broken =
            make_simple_file(unresolved, "a.proto", "app");
        add_message_with_field(broken, "A", "v", 1, ".shared.Absent");
    }
    CHECK(!easypb_schema::link_schema(unresolved,
                                      easypb_schema::LINK_COMPLETE, reused,
                                      error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
    CHECK(reused.files == 0);
    CHECK(!reused.local_only);
    CHECK(reused.symbols.symbols.empty());
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
    const std::vector<std::int32_t> index_zero = repeated_indexes(1, 0);

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
    // The enum carries a value since fix round r16: `add_enum(b, "Mix", 0)`
    // declared an enum with no values, which is a shape protoc refuses and
    // D-32 closed - the reference-free enum sweep now refuses it BEFORE the
    // stage-3 index this test is about, so the fixture had to become a shape
    // protoc would build. The collision under test is unchanged: the enum VALUE
    // lives in the containing scope, the enum TYPE collides with a's message.
    add_enum(b, "Mix", "MIX_VALUE");

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
    // field's key is qualified by its containing scope. a.proto imports
    // b.proto so the fixture's field really resolves from Step 5 on - the
    // index keys are the subject here, not a visibility failure.
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& a = make_file(
        set, "a.proto", std::vector<std::string>(1, "b.proto"), no_indexes,
        no_indexes);
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

// D-05: a conflicting declaration is ONE category, so it carries ONE code by
// whichever producer catches it - the scope validation that runs inside a file
// before anything is indexed, or the symbol insertion that sees the collision
// between two files. Oracle (libprotoc 30.1, cases E307/E308) rejects both
// shapes but words them differently ("already defined in \"pkg\"" against
// "already defined in file \"a.proto\""), so the shared contract has to be
// the code, never the text (E1/A3).
void test_conflicting_declaration_reports_one_code()
{
    easypb_schema::DiagnosticCode codes[2];
    easypb_schema::Diagnostic errors[2];

    // Producer 1: both declarations in the same file and the same scope.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
        add_message(file, "Same");
        add_message(file, "Same");

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                          linked, error));
        codes[0] = error.code;
        errors[0] = error;
        CHECK(error.file == "a.proto");
        CHECK(error.message.find("Same") != std::string::npos);
        CHECK(linked.files == 0);
        CHECK(!linked.local_only);
    }
    // Producer 2: the same collision split over two files.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& a = make_simple_file(set, "a.proto", "pkg");
        add_message(a, "Same");
        easypb_schema::SchemaFile& b = make_simple_file(set, "b.proto", "pkg");
        add_message(b, "Same");

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                          linked, error));
        codes[1] = error.code;
        errors[1] = error;
        CHECK(error.message.find("Same") != std::string::npos);
        CHECK(linked.files == 0);
        CHECK(!linked.local_only);
    }

    CHECK(codes[0] == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
    CHECK(codes[1] == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
    CHECK(codes[0] == codes[1]);
    // Neither message may read as a generic failure: each names the colliding
    // declaration, and the cross-file one names both files as well.
    CHECK(errors[0].message.find("name collision") != std::string::npos);
    CHECK(errors[1].message.find("already defined") != std::string::npos);
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
        CHECK(link_verdict(set, error));
        CHECK(text(chain.a->file.message_type[0].field[0].type_name) ==
              ".shared.Value");
        CHECK(chain.a->file.message_type[0].field[0].type ==
              FieldDescriptorProto::TYPE_MESSAGE);

        // c's symbol: known to the compilation, invisible from a.proto.
        retype_field(*chain.a, 0, 0, FieldDescriptorProto::LABEL_OPTIONAL,
                     "shared.Other");
        error = easypb_schema::Diagnostic();
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);
        CHECK(error.file == "a.proto");
        CHECK(error.message.find(".shared.Other") != std::string::npos);
        CHECK(error.message.find("c.proto") != std::string::npos);
        CHECK(error.message.find("import") != std::string::npos);

        // A name no file declares: the same walk, the other category.
        retype_field(*chain.a, 0, 0, FieldDescriptorProto::LABEL_OPTIONAL,
                     "shared.Absent");
        error = easypb_schema::Diagnostic();
        CHECK(!link_verdict(set, error));
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
        CHECK(link_verdict(set, error));
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
    CHECK(!link_verdict(set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
    CHECK(error.message.find("unresolved type Choice.Item") != std::string::npos);
    CHECK(error.message.find("\".pkg.Encloser.Choice.Item\"") !=
          std::string::npos);
    CHECK(error.message.find("resolved to") != std::string::npos);

    // B2: the absolute reference to the intended outer type still passes, and
    // it needs no outward fallback to get there.
    retype_nested_field(file, 1, 1, 0, ".pkg.Choice.Item");
    error = easypb_schema::Diagnostic();
    CHECK(link_verdict(set, error));
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
        CHECK(link_verdict(set, error));
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
        CHECK(link_verdict(set, error));
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
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_NOT_A_TYPE);
        CHECK(error.message.find("\"Thing\"") != std::string::npos);
        CHECK(error.message.find("a field") != std::string::npos);
    }
}

// Fix round r10, D-21: the head rule is TWO filters, because protoc's
// `Symbol::IsAggregate()` and `Symbol::IsType()` differ. A SERVICE may commit
// the head of a COMPOUND reference (it is an aggregate) and then fails the
// member lookup - that failure is protoc's verdict for the shape (oracle
// `head01`); it may never win a WHOLE-name reference (it is not a type), so
// an outer message of the same name still wins (`head03`). The same split is
// what makes a visible PACKAGE scope skip a whole name instead of shadowing
// an outer type (`head04`, the mirror half a single `kind != 0` predicate got
// wrong in both directions).
void test_service_and_package_heads_follow_the_two_filters()
{
    // head01: package inner; import dep.proto; service Q; optional Q.R f = 1.
    // The dependency declares the global message Q with the nested R, so an
    // outward fallback would ACCEPT; the committed service head must not
    // reach it.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& dep = make_simple_file(set, "dep.proto", 0);
        {
            DescriptorProto q;
            q.name = dep.strings.save("Q");
            q.has_name = true;
            add_nested(dep, q, "R");
            dep.file.message_type.push_back(q);
        }
        easypb_schema::SchemaFile& root = make_file(
            set, "root.proto", std::vector<std::string>(1, "dep.proto"),
            std::vector<std::int32_t>(), std::vector<std::int32_t>());
        set_package(root, "inner");
        add_service(root, "Q");
        {
            DescriptorProto use;
            use.name = root.strings.save("Use");
            use.has_name = true;
            add_field(root, use, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                      "Q.R");
            root.file.message_type.push_back(use);
        }

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        CHECK(error.message.find("unresolved type Q.R in ") != std::string::npos);
        CHECK(error.message.find("\".inner.Q.R\"") != std::string::npos);
        CHECK(error.message.find("resolved to") != std::string::npos);
        CHECK(error.message.find("not defined") != std::string::npos);

        // Control: the absolute spelling reaches the outer message directly
        // and links - the refusal above is the committed service head, not a
        // missing symbol.
        root.file.message_type[0].field[0].type_name =
            root.strings.save(".Q.R");
        root.file.message_type[0].field[0].has_type_name = true;
        error = easypb_schema::Diagnostic();
        CHECK(link_verdict(set, error));
        CHECK(text(root.file.message_type[0].field[0].type_name) == ".Q.R");
    }

    // head03: the same two files with a WHOLE-name reference. The nearer
    // service is not a type, so the walk continues to the outer message and
    // the reference links (protoc accepts).
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& dep = make_simple_file(set, "dep.proto", 0);
        add_message(dep, "Q");
        easypb_schema::SchemaFile& root = make_file(
            set, "root.proto", std::vector<std::string>(1, "dep.proto"),
            std::vector<std::int32_t>(), std::vector<std::int32_t>());
        set_package(root, "inner");
        add_service(root, "Q");
        {
            DescriptorProto use;
            use.name = root.strings.save("Use");
            use.has_name = true;
            add_field(root, use, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                      "Q");
            root.file.message_type.push_back(use);
        }
        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        CHECK(text(root.file.message_type[0].field[0].type_name) == ".Q");
        CHECK(root.file.message_type[0].field[0].type ==
              FieldDescriptorProto::TYPE_MESSAGE);
    }

    // head04: a visible PACKAGE scope must not shadow an outer message either.
    // `package acme.core.bar` is a nearer candidate than the message
    // `.acme.bar`, and the whole-name reference has to skip it (protoc
    // accepts and even reports the nearer file's import as unused).
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& main = make_simple_file(set, "main.proto",
                                                          "acme");
        add_message(main, "bar");
        easypb_schema::SchemaFile& deep = make_simple_file(
            set, "deep.proto", "acme.core.bar");
        (void)deep;
        std::vector<std::string> deps;
        deps.push_back("deep.proto");
        deps.push_back("main.proto");
        easypb_schema::SchemaFile& root = make_file(
            set, "root.proto", deps, std::vector<std::int32_t>(),
            std::vector<std::int32_t>());
        set_package(root, "acme.core");
        {
            DescriptorProto uses;
            uses.name = root.strings.save("Uses");
            uses.has_name = true;
            add_field(root, uses, "b", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                      "bar");
            root.file.message_type.push_back(uses);
        }
        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        CHECK(text(root.file.message_type[0].field[0].type_name) ==
              ".acme.bar");
    }

    // head05: without the outer declarer the same reference is refused, and
    // the first candidate the failure walk knows is the visible package
    // scope: the plan's category for a whole-name non-type match stays
    // NOT_A_TYPE. protoc's verdict agrees; its wording is "is not defined", a
    // wording divergence our one-code-per-outcome rule keeps (E1/A3, the
    // B9/B15 pattern).
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& deep = make_simple_file(
            set, "deep.proto", "acme.core.bar");
        (void)deep;
        easypb_schema::SchemaFile& root = make_file(
            set, "root.proto", std::vector<std::string>(1, "deep.proto"),
            std::vector<std::int32_t>(), std::vector<std::int32_t>());
        set_package(root, "acme.core");
        {
            DescriptorProto uses;
            uses.name = root.strings.save("Uses");
            uses.has_name = true;
            add_field(root, uses, "b", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                      "bar");
            root.file.message_type.push_back(uses);
        }
        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_NOT_A_TYPE);
        CHECK(error.message.find("a package scope") != std::string::npos);
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
        CHECK(link_verdict(set, error));
        CHECK(text(root.file.message_type[0].field[0].type_name) == ".sub.X");

        root.file.dependency.push_back(root.strings.save("hidden.proto"));
        retype_field(root, 0, 0, FieldDescriptorProto::LABEL_OPTIONAL, "sub.X");
        error = easypb_schema::Diagnostic();
        CHECK(!link_verdict(set, error));
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
        CHECK(link_verdict(set, error));
        CHECK(text(root.file.message_type[0].field[0].type_name) == ".sub.X");

        root.file.dependency.push_back(root.strings.save("hidden.proto"));
        retype_field(root, 0, 0, FieldDescriptorProto::LABEL_OPTIONAL, "sub.X");
        error = easypb_schema::Diagnostic();
        CHECK(!link_verdict(set, error));
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
    CHECK(link_verdict(set, error));
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
        CHECK(link_verdict(sub_set, sub_error));
        CHECK(text(sub_root.file.message_type[0].field[0].type_name) ==
              ".shared.Value");
    }
}

// Oracle F5, re-derived by fix round r5: a visible file that declares only
// the SUB-package `shared.deep` offers no `Value` under `shared`, so
// `shared.Value` fails. The sub-package declarer DOES cover the head `shared`
// - protoc's reference rule is a prefix test with a dot boundary
// (`descriptor.cc` IsInPackage; oracle rows r01..r07 of the code-review
// corpus) - which commits the package head; the member `.shared.Value` is not
// indexed, so the residual category is the not-defined one, exactly the class
// protoc reports for this shape (`"shared.Value" is not defined`, oracle
// b01, `build/plan4/fix-r5/oracle/oracle-summary.txt`). The equality rule
// this test was written under answered *invisible* here and named deep.proto;
// that reading was the D-14 defect.
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
    CHECK(!link_verdict(set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
    CHECK(error.message.find("shared.Value") != std::string::npos);
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
    CHECK(!link_verdict(set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
    CHECK(error.message.find("SomeEnum.VALUE") != std::string::npos);
    CHECK(error.message.find("\".pkg.SomeEnum.VALUE\"") != std::string::npos);

    retype_field(file, 0, 0, FieldDescriptorProto::LABEL_OPTIONAL, "VALUE");
    error = easypb_schema::Diagnostic();
    CHECK(!link_verdict(set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_NOT_A_TYPE);
    CHECK(error.message.find("\"VALUE\"") != std::string::npos);
    CHECK(error.message.find("an enum value") != std::string::npos);
}

// Holder with one member of every non-type category the index can hold, plus a
// real nested type: the shape that tells a "found, but not a type" member
// apart from a member the index does not know at all.
void make_holder_fixture(easypb_schema::SchemaFile& file)
{
    DescriptorProto holder;
    holder.name = file.strings.save("Holder");
    holder.has_name = true;
    add_field(file, holder, "not_a_type", 1,
              FieldDescriptorProto::LABEL_OPTIONAL, 0);

    OneofDescriptorProto oneof;
    oneof.name = file.strings.save("pick");
    oneof.has_name = true;
    holder.oneof_decl.push_back(oneof);
    // The declaration needs a member: an unclaimed oneof is the shape D-28's
    // check refuses, protoc refuses it too (`Oneof must have at least one
    // field.`), so a fixture carrying one would be a descriptor no reference
    // compiler accepts. The member is the fixture's second non-type symbol,
    // which keeps the kind census in
    // test_indexed_symbol_kinds_stay_in_the_documented_domain meaningful.
    add_field(file, holder, "pick_member", 2,
              FieldDescriptorProto::LABEL_OPTIONAL, 0);
    holder.field[1].oneof_index = 0;
    holder.field[1].has_oneof_index = true;

    EnumDescriptorProto enumeration;
    enumeration.name = file.strings.save("E");
    enumeration.has_name = true;
    EnumValueDescriptorProto value;
    value.name = file.strings.save("V");
    value.has_name = true;
    value.number = 0;
    value.has_number = true;
    enumeration.value.push_back(value);
    holder.enum_type.push_back(enumeration);

    add_nested(file, holder, "N");
    file.file.message_type.push_back(holder);
}

// D-01: a compound relative name whose member the index KNOWS but which is not
// a type reports the same code as the absolute spelling of that declaration -
// one category, one code (E1/A3) - while a member nothing is indexed under
// stays "resolved to ... which is not defined", and a real type still
// resolves. Oracle (libprotoc 30.1, cases E300-E306): `Holder.not_a_type`,
// `Holder.pick` and `Holder.V` are all "is not a type", `Holder.E.V` is
// "resolved to \"pkg.Holder.E.V\", which is not defined" because the value is
// indexed in the enum's CONTAINING scope, and `Holder.N` is accepted.
void test_compound_member_non_type_matches_absolute_spelling()
{
    struct Case {
        const char* reference;
        easypb_schema::DiagnosticCode code;
        const char* category;
    };
    const Case cases[] = {
        // A field, a oneof and an enum value in its containing scope: three
        // spellings of "the member exists and is not a type".
        {"Holder.not_a_type", easypb_schema::DIAGNOSTIC_NOT_A_TYPE,
         "a field"},
        {".pkg.Holder.not_a_type", easypb_schema::DIAGNOSTIC_NOT_A_TYPE,
         "a field"},
        {"Holder.pick", easypb_schema::DIAGNOSTIC_NOT_A_TYPE, "a oneof"},
        {".pkg.Holder.pick", easypb_schema::DIAGNOSTIC_NOT_A_TYPE,
         "a oneof"},
        {"Holder.V", easypb_schema::DIAGNOSTIC_NOT_A_TYPE, "an enum value"},
        {".pkg.Holder.V", easypb_schema::DIAGNOSTIC_NOT_A_TYPE,
         "an enum value"},
        // Nothing is indexed under the enum's own path, so this member really
        // is absent and keeps the compound-path wording.
        {"Holder.E.V", easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE,
         "which is not defined"},
        {"Holder.absent_member", easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE,
         "which is not defined"}
    };

    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
        make_holder_fixture(file);
        DescriptorProto requester;
        requester.name = file.strings.save("R");
        requester.has_name = true;
        add_field(file, requester, "bad", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, cases[i].reference);
        file.file.message_type.push_back(requester);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == cases[i].code);
        CHECK(error.message.find(cases[i].category) != std::string::npos);
        // Both the relative and the absolute row name the resolved path, so
        // the diagnostic does not read as a plain typo.
        CHECK(error.message.find("\".pkg.") != std::string::npos);
    }

    // The real nested type of the same fixture still resolves: the fix moves
    // the classification of a non-type member, it does not narrow the walk.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
        make_holder_fixture(file);
        DescriptorProto requester;
        requester.name = file.strings.save("R");
        requester.has_name = true;
        add_field(file, requester, "bad", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "Holder.N");
        file.file.message_type.push_back(requester);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        CHECK(text(file.file.message_type[1].field[0].type_name) ==
              ".pkg.Holder.N");
        CHECK(file.file.message_type[1].field[0].type ==
              FieldDescriptorProto::TYPE_MESSAGE);
    }
}

// --- D-06 ------------------------------------------------------------------
// A package scope reached as a TYPE must report one code however it is spelled
// and whichever branch of the walk reaches it. The rule is R4: a merged package
// node is visible when ANY visible file declares exactly that package, not when
// the file the index recorded first - the node's owner - is visible. Both
// branches tested the owner, so the relative spelling, which asks R4, answered
// NOT_A_TYPE while the absolute spelling answered INVISIBLE for the same
// declaration, naming a file the requester never imported.
//
// The fixtures mirror the measured oracle cases in build/plan4/fix-r2/oracle
// (libprotoc 30.1): the merged corner, where an unimported first-indexed file
// and an imported file declare the same package (protoc: ".game.deep" is not a
// type), the same question through the compound-member path, where the head
// stays visible while the member is a package node owned by the unimported
// file (protoc: "deep.sub" is not a type), and the inverse controls - no
// visible declarer keeps INVISIBLE, and a real message under the member package
// still resolves.
easypb_schema::SchemaFile& make_reference_requester(
    easypb_schema::SchemaSet& set, const std::vector<std::string>& deps,
    const char* reference)
{
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaFile& file = make_file(set, "a.proto", deps, no_indexes,
                                                no_indexes);
    set_package(file, "game");
    DescriptorProto ask;
    ask.name = file.strings.save("Ask");
    ask.has_name = true;
    add_field(file, ask, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL, reference);
    file.file.message_type.push_back(ask);
    return file;
}

// h.proto declares the package and is never imported, and it is indexed first,
// so the merged node's recorded owner is an invisible file; z.proto, imported,
// declares the same package, which is what keeps the node visible under R4.
void make_merged_package_fixture(easypb_schema::SchemaSet& set,
                                 easypb_schema::SchemaFile*& requester,
                                 const char* reference)
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaFile& a = make_reference_requester(
        set, std::vector<std::string>(1, "z.proto"), reference);
    easypb_schema::SchemaFile& unimported =
        make_file(set, "h.proto", no_deps, no_indexes, no_indexes);
    set_package(unimported, "game.deep");
    easypb_schema::SchemaFile& imported =
        make_file(set, "z.proto", no_deps, no_indexes, no_indexes);
    set_package(imported, "game.deep");
    add_message(imported, "Msg");
    requester = &a;
}

// The compound half: y.proto declares exactly `package game.deep` (the head the
// walk must keep), h.proto and, when visible_sub, z.proto declare exactly
// `package game.deep.sub`. h.proto is indexed first and never imported, so it
// owns the member node; with visible_sub false nothing visible declares that
// package, which is the control.
void make_chain_package_fixture(easypb_schema::SchemaSet& set,
                                easypb_schema::SchemaFile*& requester,
                                const char* reference, bool visible_sub)
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;
    std::vector<std::string> deps(1, "y.proto");
    if (visible_sub) deps.push_back("z.proto");
    easypb_schema::SchemaFile& a =
        make_reference_requester(set, deps, reference);
    easypb_schema::SchemaFile& unimported =
        make_file(set, "h.proto", no_deps, no_indexes, no_indexes);
    set_package(unimported, "game.deep.sub");
    add_message(unimported, "HiddenOnly");
    easypb_schema::SchemaFile& head =
        make_file(set, "y.proto", no_deps, no_indexes, no_indexes);
    set_package(head, "game.deep");
    if (visible_sub) {
        easypb_schema::SchemaFile& member =
            make_file(set, "z.proto", no_deps, no_indexes, no_indexes);
        set_package(member, "game.deep.sub");
        add_message(member, "VisibleOnly");
        add_message(member, "Thing");
    }
    requester = &a;
}

// No visible file declares the package at all: the inverse control.
void make_unimported_package_fixture(easypb_schema::SchemaSet& set,
                                     easypb_schema::SchemaFile*& requester,
                                     const char* reference)
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaFile& a = make_reference_requester(set, no_deps,
                                                            reference);
    easypb_schema::SchemaFile& unimported =
        make_file(set, "h.proto", no_deps, no_indexes, no_indexes);
    set_package(unimported, "game.deep");
    add_message(unimported, "Msg");
    requester = &a;
}

// One expectation: `expected_code` is -1 when the reference must resolve, and
// `expected_interned` is the name the field must carry once it does.
void check_reference_case(easypb_schema::SchemaSet& set,
                          easypb_schema::SchemaFile& requester,
                          int expected_code, const char* expected_substr,
                          const char* expected_interned)
{
    easypb_schema::Diagnostic error;
    const bool resolved = link_verdict(set, error);
    CHECK(resolved == (expected_code < 0));
    if (!resolved && expected_code >= 0) CHECK(error.code == expected_code);
    if (!resolved && expected_substr != 0) {
        CHECK(error.message.find(expected_substr) != std::string::npos);
    }
    if (resolved && expected_interned != 0) {
        CHECK(text(requester.file.message_type[0].field[0].type_name) ==
              expected_interned);
    }
}

void test_merged_package_node_is_not_a_type_in_both_spellings()
{
    // The merged node as a type: the relative spelling asks R4, and the absolute
    // spelling must agree instead of naming the unimported owner.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile* requester = 0;
        make_merged_package_fixture(set, requester, "deep");
        check_reference_case(set, *requester,
                             easypb_schema::DIAGNOSTIC_NOT_A_TYPE,
                             "a package scope", 0);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile* requester = 0;
        make_merged_package_fixture(set, requester, ".game.deep");
        check_reference_case(set, *requester,
                             easypb_schema::DIAGNOSTIC_NOT_A_TYPE,
                             "a package scope", 0);
    }
    // The compound-member branch, same question: the visible head commits the
    // name and the member is the package node the unimported file owns.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile* requester = 0;
        make_chain_package_fixture(set, requester, "deep.sub", true);
        check_reference_case(set, *requester,
                             easypb_schema::DIAGNOSTIC_NOT_A_TYPE,
                             "a package scope", 0);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile* requester = 0;
        make_chain_package_fixture(set, requester, ".game.deep.sub", true);
        check_reference_case(set, *requester,
                             easypb_schema::DIAGNOSTIC_NOT_A_TYPE,
                             "a package scope", 0);
    }
    // Controls: with no visible declarer the node stays invisible, and the two
    // spellings still agree - the fix moved the rule, not the category.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile* requester = 0;
        make_unimported_package_fixture(set, requester, "deep");
        check_reference_case(set, *requester,
                             easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE,
                             "h.proto", 0);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile* requester = 0;
        make_unimported_package_fixture(set, requester, ".game.deep");
        check_reference_case(set, *requester,
                             easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE,
                             "h.proto", 0);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile* requester = 0;
        make_chain_package_fixture(set, requester, "deep.sub", false);
        check_reference_case(set, *requester,
                             easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE,
                             "h.proto", 0);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile* requester = 0;
        make_chain_package_fixture(set, requester, ".game.deep.sub", false);
        check_reference_case(set, *requester,
                             easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE,
                             "h.proto", 0);
    }
    // Accept controls: the walk still reaches a real message under the member
    // package, in both spellings.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile* requester = 0;
        make_chain_package_fixture(set, requester, "deep.sub.Thing", true);
        check_reference_case(set, *requester, -1, 0, ".game.deep.sub.Thing");
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile* requester = 0;
        make_chain_package_fixture(set, requester, ".game.deep.sub.Thing", true);
        check_reference_case(set, *requester, -1, 0, ".game.deep.sub.Thing");
    }
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

    // One link resolves BOTH files: stage 5 walks every file, not a chosen
    // root, so the cross-file reference (X -> y.proto's Y) and the remote
    // file's own self-reference are answered by the same pass.
    easypb_schema::Diagnostic error;
    CHECK(link_verdict(set, error));
    CHECK(text(root.file.message_type[0].field[0].type_name) == ".pkg.Node");
    CHECK(text(root.file.message_type[1].field[0].type_name) == ".pkg.B");
    CHECK(text(root.file.message_type[2].field[0].type_name) == ".pkg.A");
    CHECK(text(root.file.message_type[3].field[0].type_name) == ".q.Y");
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
        CHECK(link_verdict(set, error));
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
        CHECK(link_verdict(set, error));
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
    CHECK(!link_verdict(set, error));
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
        CHECK(set.find_file("a.proto") != 0);
        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
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
        CHECK(!link_verdict(set, error));
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
        CHECK(!link_verdict(set, error));
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
    CHECK(!link_verdict(set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);
    CHECK(error.message.find("hidden.proto") != std::string::npos);
    CHECK(error.message.find("map entry") == std::string::npos);
}

// The synthesized entry's value field is resolved from its own reference, not
// through the map field's synthetic entry name: the other route would silently
// defeat the cross-syntax check Step 6.2 adds (the classification itself is
// Step 6's, so this test asserts the precondition only). The entry is
// descriptor-origin, so its value member must DECLARE the kind it names (A7):
// an enum-valued entry that claims a message is a kind mismatch, not a
// resolution, from Step 5.1 on.
void test_map_entry_value_resolves_from_its_own_token()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
    file.from_descriptor_set = true;
    add_enum(file, "SomeEnum", "ZERO");
    DescriptorProto m;
    m.name = file.strings.save("M");
    m.has_name = true;
    add_map_entry(file, m, "FooEntry", ".pkg.SomeEnum",
                  FieldDescriptorProto::TYPE_ENUM);
    add_map_field(file, m, "foo", 1, ".pkg.M.FooEntry");
    file.file.message_type.push_back(m);

    easypb_schema::Diagnostic error;
    CHECK(link_verdict(set, error));
    CHECK(text(file.file.message_type[0].field[0].type_name) ==
          ".pkg.M.FooEntry");
    const FieldDescriptorProto& value =
        file.file.message_type[0].nested_type[0].field[1];
    CHECK(value.has_type && value.type == FieldDescriptorProto::TYPE_ENUM);
    CHECK(text(value.type_name) == ".pkg.SomeEnum");
}

// --- Step 5: the complete-mode link loop -----------------------------------
// The cases below drive link_schema itself. Step 5 wired stage 5 into it, so a
// resolution failure IS the link's failure and nothing is published for it;
// the Step-3/4 tests above call the same helper, link_verdict.

easypb_schema::SourceLocation at_line(int line)
{
    easypb_schema::SourceLocation location;
    location.offset = 0;
    location.line = static_cast<std::size_t>(line);
    location.column = 1;
    return location;
}

// Records the per-field source provenance a parser would record, so a failure
// can be asserted AT the token the contract names instead of at a descriptor
// path. Fixtures without this stay descriptor-only and fall back to the path.
void record_field_source(easypb_schema::SchemaFile& file,
                         const easypb_schema::DescriptorPath& path,
                         const char* raw_type_name, int type_line,
                         int default_line, int packed_line)
{
    easypb_schema::FieldSource source;
    source.path = path;
    if (raw_type_name != 0) source.raw_type_name = raw_type_name;
    source.type_location =
        type_line > 0 ? at_line(type_line) : easypb_schema::unknown_location();
    source.default_location = default_line > 0
                                  ? at_line(default_line)
                                  : easypb_schema::unknown_location();
    source.packed_location = packed_line > 0
                                 ? at_line(packed_line)
                                 : easypb_schema::unknown_location();
    file.field_sources.push_back(source);
}

// The plan-4 Task 2 regression pair, in memory, with the enumeration values the
// contract fixes (READY = 7, STOPPED = 9):
//
//   state.proto  package shared; enum State { READY = 7; STOPPED = 9; }
//   report.proto package app; imports state.proto;
//                message Report {
//                  repeated shared.State states = 1 [packed = true];
//                  optional shared.State state = 2 [default = STOPPED];
//                }
//
// Both files are source-origin, so their named fields carry no declared kind
// (deferred parsing leaves has_type unset) and resolution owes them both the
// kind and the absolute name. The recorded token lines are what the two
// mutations below assert against: the pack requires the default failure at
// the default token, and the packed failure at the packed option.
struct StateReport
{
    easypb_schema::SchemaFile* state;
    easypb_schema::SchemaFile* report;
};

StateReport make_state_report_fixture(easypb_schema::SchemaSet& set,
                                      bool state_is_message)
{
    easypb_schema::SchemaFile& state =
        make_simple_file(set, "state.proto", "shared");
    if (state_is_message) {
        add_message(state, "State");
    } else {
        EnumDescriptorProto enumeration;
        enumeration.name = state.strings.save("State");
        enumeration.has_name = true;
        const char* names[2] = { "READY", "STOPPED" };
        const int numbers[2] = { 7, 9 };
        for (int i = 0; i < 2; ++i) {
            EnumValueDescriptorProto value;
            value.name = state.strings.save(names[i]);
            value.has_name = true;
            value.number = numbers[i];
            value.has_number = true;
            enumeration.value.push_back(value);
        }
        state.file.enum_type.push_back(enumeration);
    }

    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaFile& report = make_file(
        set, "report.proto", std::vector<std::string>(1, "state.proto"),
        no_indexes, no_indexes);
    set_package(report, "app");
    DescriptorProto message;
    message.name = report.strings.save("Report");
    message.has_name = true;
    {
        FieldDescriptorProto field;
        field.name = report.strings.save("states");
        field.has_name = true;
        field.number = 1;
        field.has_number = true;
        field.label = FieldDescriptorProto::LABEL_REPEATED;
        field.has_label = true;
        field.type_name = report.strings.save("shared.State");
        field.has_type_name = true;
        field.options.packed = true;
        field.options.has_packed = true;
        message.field.push_back(field);
    }
    {
        FieldDescriptorProto field;
        field.name = report.strings.save("state");
        field.has_name = true;
        field.number = 2;
        field.has_number = true;
        field.label = FieldDescriptorProto::LABEL_OPTIONAL;
        field.has_label = true;
        field.type_name = report.strings.save("shared.State");
        field.has_type_name = true;
        field.default_value = report.strings.save("STOPPED");
        field.has_default_value = true;
        message.field.push_back(field);
    }
    report.file.message_type.push_back(message);

    const easypb_schema::DescriptorPath message_path =
        easypb_schema::file_message_path(0);
    record_field_source(report,
                        easypb_schema::message_field_path(message_path, 0),
                        "shared.State", 5, 0, 5);
    record_field_source(report,
                        easypb_schema::message_field_path(message_path, 1),
                        "shared.State", 6, 6, 0);

    StateReport pair = { &state, &report };
    return pair;
}

void test_task2_pair_links_and_its_two_mutations_fail()
{
    // Intact: both fields resolve to the imported enum, with its kind and its
    // absolute name, and the link publishes completely.
    {
        easypb_schema::SchemaSet set;
        const StateReport pair = make_state_report_fixture(set, false);
        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        const FieldDescriptorProto& states =
            pair.report->file.message_type[0].field[0];
        CHECK(states.has_type && states.type == FieldDescriptorProto::TYPE_ENUM);
        CHECK(text(states.type_name) == ".shared.State");
        const FieldDescriptorProto& state =
            pair.report->file.message_type[0].field[1];
        CHECK(state.has_type && state.type == FieldDescriptorProto::TYPE_ENUM);
        CHECK(text(state.type_name) == ".shared.State");
    }
    // Mutation A: the default names an enumerator the enum does not define.
    // protoc reports it at the default token, and so do we: the recorded
    // default line is what the diagnostic carries.
    {
        easypb_schema::SchemaSet set;
        const StateReport pair = make_state_report_fixture(set, false);
        pair.report->file.message_type[0].field[1].default_value =
            pair.report->strings.save("STARTED");
        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.file == "report.proto");
        CHECK(error.location.line == 6);
        CHECK(error.message.find(".shared.State") != std::string::npos);
        CHECK(error.message.find("STARTED") != std::string::npos);
    }
    // Mutation B: the imported definition becomes a message, so the packed
    // field is rejected at the packed option. This is NOT the kind-mismatch
    // case: the field is source-origin, so it declares no kind and the packed
    // check fires first (A7 is tested with a declared kind below).
    {
        easypb_schema::SchemaSet set;
        make_state_report_fixture(set, true);
        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.file == "report.proto");
        CHECK(error.location.line == 5);
        CHECK(error.message.find("packed") != std::string::npos);
    }
}

// A7: a descriptor-origin field's declared kind is authoritative metadata, so
// the resolver COMPARES it with the definition its name resolved to. An enum
// declaration that names a message must fail with DIAGNOSTIC_KIND_MISMATCH,
// and the failure must not have rewritten the metadata it refused - which is
// why the check lives before `field.type = resolved->kind`. The mirror case
// (declares a message, names an enum) is the same rule from the other side,
// and matching kinds still link: the rule is a comparison, not a rejection of
// descriptor-origin input.
void test_descriptor_origin_declared_kind_is_compared_not_rewritten()
{
    // Declares an enum, names a message.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "Holder");
        DescriptorProto r;
        r.name = file.strings.save("R");
        r.has_name = true;
        add_field(file, r, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  ".pkg.Holder");
        r.field[0].type = FieldDescriptorProto::TYPE_ENUM;
        file.file.message_type.push_back(r);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_KIND_MISMATCH);
        CHECK(error.file == "a.proto");
        CHECK(error.message.find("\"f\"") != std::string::npos);
        CHECK(error.message.find(".pkg.Holder") != std::string::npos);
        CHECK(error.message.find("an enum") != std::string::npos);
        CHECK(error.message.find("a message") != std::string::npos);
        // The declared kind and the raw name survive the refusal.
        CHECK(r.field[0].has_type);
        CHECK(r.field[0].type == FieldDescriptorProto::TYPE_ENUM);
        CHECK(text(r.field[0].type_name) == ".pkg.Holder");
    }
    // Declares a message, names an enum.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
        file.from_descriptor_set = true;
        add_enum(file, "Holder", "ZERO");
        DescriptorProto r;
        r.name = file.strings.save("R");
        r.has_name = true;
        add_field(file, r, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  ".pkg.Holder");
        file.file.message_type.push_back(r);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_KIND_MISMATCH);
        CHECK(error.message.find("a message") != std::string::npos);
        CHECK(error.message.find("an enum") != std::string::npos);
        CHECK(r.field[0].type == FieldDescriptorProto::TYPE_MESSAGE);
    }
    // Matching kinds: an enum declared as an enum.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
        file.from_descriptor_set = true;
        add_enum(file, "Holder", "ZERO");
        DescriptorProto r;
        r.name = file.strings.save("R");
        r.has_name = true;
        add_field(file, r, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  ".pkg.Holder");
        r.field[0].type = FieldDescriptorProto::TYPE_ENUM;
        file.file.message_type.push_back(r);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        CHECK(r.field[0].type == FieldDescriptorProto::TYPE_ENUM);
        CHECK(text(r.field[0].type_name) == ".pkg.Holder");
    }
}

// D-23 (contract clause 12): a descriptor-origin field number must be in
// 1..536870911 and outside the reserved 19000..19999 range, enforced over
// every available field in both modes, referenced or not (A26's entry-shape
// precedent) and before any resolution. The source path cannot produce these
// shapes - the parser refuses them as source text, pinned by test_parser.cpp's
// test_rejects_invalid_field_numbers - and the pre-fix measurement that raised
// the row is build/plan4/code-review-0927-ds/probe: the same shapes printed
// ACCEPTED from the complete linker.
void test_descriptor_origin_field_numbers_are_validated()
{
    const struct { int number; const char* sentence; } bad[] = {
        {0, "field \"f\" number 0 is outside the valid range 1..536870911"},
        {19000, "field \"f\" number 19000 is inside the reserved range 19000..19999"},
        {19999, "field \"f\" number 19999 is inside the reserved range 19000..19999"},
        {536870912, "field \"f\" number 536870912 is outside the valid range 1..536870911"}
    };
    for (std::size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "gap.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_field(file, holder, "f", bad[i].number,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        file.file.message_type.push_back(holder);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                          linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_FIELD_NUMBER);
        CHECK(error.file == "gap.pbs");
        CHECK(error.message.find(bad[i].sentence) == 0);
        // Descriptor-origin input has no source location, so the diagnostic
        // appends the descriptor path (fail_at_code's convention).
        CHECK(error.message.find(" at [") != std::string::npos);
        CHECK(linked.files == 0);
        CHECK(!linked.local_only);
    }
    // The legal bounds on both sides stay legal: number 1, one below the
    // reserved range, one above it, and the maximum itself.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "ok.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_field(file, holder, "a", 1, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        add_field(file, holder, "b", 18999, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        add_field(file, holder, "c", 20000, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        add_field(file, holder, "d", 536870911,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        file.file.message_type.push_back(holder);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
    }
}

// D-23 (contract clause 12): a field that declares a named kind - MESSAGE,
// ENUM or GROUP - must carry a type_name, because the generator emits the name
// the descriptor gives it (`codegen/codegen.cpp:234` reads `field.type_name`)
// and an empty one becomes a broken qualified name. protoc's cross-link
// refuses the same shape ("Field with message or enum type missing
// type_name."), and a group is a message. The rule keys on a DECLARED kind: a
// scalar kind without a type_name stays legal, and the parser's deferred
// output (has_type_name set, has_type not yet) stays legal too - the name is
// there, only the kind is left to the link.
void test_descriptor_origin_named_kind_needs_type_name()
{
    const struct { int type; const char* category; } named[] = {
        {FieldDescriptorProto::TYPE_MESSAGE, "a message"},
        {FieldDescriptorProto::TYPE_ENUM, "an enum"},
        {FieldDescriptorProto::TYPE_GROUP, "a group"}
    };
    for (std::size_t i = 0; i < sizeof(named) / sizeof(named[0]); ++i) {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "gap.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_field(file, holder, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        // Keep the declared kind and leave the name unset: the shape protoc
        // refuses. The scalar type add_field wrote is overwritten, not the
        // name, which was never set at all.
        holder.field[0].type = named[i].type;
        file.file.message_type.push_back(holder);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                          linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_TYPE_NAME);
        CHECK(error.file == "gap.pbs");
        const std::string sentence =
            std::string("field \"f\" declares ") + named[i].category +
            " type but carries no type_name";
        CHECK(error.message.find(sentence) == 0);
        CHECK(error.message.find(" at [") != std::string::npos);
        CHECK(linked.files == 0);
        CHECK(!linked.local_only);
    }
    // A scalar kind with no type_name is the ordinary case and links.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "ok.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_field(file, holder, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        file.file.message_type.push_back(holder);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
    }
    // A declared named kind WITH its type_name links: the rule is the pair's
    // coherence, not a ban on named kinds.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "ok.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "Target");
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_field(file, holder, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  ".pkg.Target");
        file.file.message_type.push_back(holder);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
    }
    // The parser's deferred output shape: has_type_name is set, has_type is
    // not. The named-kind rule must not read it as a missing type_name, and
    // the link resolves it to the local message.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "source.proto", "pkg");
        add_message(file, "Target");
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        FieldDescriptorProto deferred;
        deferred.name = file.strings.save("f");
        deferred.has_name = true;
        deferred.number = 1;
        deferred.has_number = true;
        deferred.label = FieldDescriptorProto::LABEL_OPTIONAL;
        deferred.has_label = true;
        deferred.type_name = file.strings.save("Target");
        deferred.has_type_name = true;
        holder.field.push_back(deferred);
        file.file.message_type.push_back(holder);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        const FieldDescriptorProto& linked_field =
            file.file.message_type[1].field[0];
        CHECK(linked_field.has_type);
        CHECK(linked_field.type == FieldDescriptorProto::TYPE_MESSAGE);
        CHECK(text(linked_field.type_name) == ".pkg.Target");
    }
}

// Appends a map-entry message with the marker but WITHOUT a value member: the
// corpus shape (`tests/codegen/maps/data/malformed_map.pbs`), which the parser
// cannot produce because it always synthesizes both members.
void add_map_entry_key_only(easypb_schema::SchemaFile& file,
                            DescriptorProto& message, const char* entry_name)
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

    message.nested_type.push_back(entry);
}

// A26: entry shape is descriptor input, not a by-product of a reference. Every
// message carrying the map_entry marker must hold a field numbered 1 and a
// field numbered 2, whether or not anything names the entry, and the failure
// must beat the resolution of a referencing field - the sweep runs first, so a
// user reference cannot re-code it as a foreign map entry. The message text is
// the generator's today, so the CLI-visible check keeps matching when plan 5
// deletes the generator's own copy of this rule.
void test_malformed_map_entry_is_rejected_without_a_reference()
{
    const char* expected =
        "Malformed map entry .easypb.test.BrokenMap.ItemsEntry: fields 1 and 2 "
        "are required";
    // Unreferenced, and named exactly like the corpus fixture the generator
    // rejects today (`tests/codegen/maps/test_codegen_maps.cmake:139`).
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "malformed_map.pbs", "easypb.test");
        file.from_descriptor_set = true;
        DescriptorProto broken;
        broken.name = file.strings.save("BrokenMap");
        broken.has_name = true;
        add_map_entry_key_only(file, broken, "ItemsEntry");
        file.file.message_type.push_back(broken);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MALFORMED_MAP_ENTRY);
        CHECK(error.file == "malformed_map.pbs");
        CHECK(error.message == expected);
    }
    // Referenced by an ordinary repeated message field: same verdict and same
    // text, because the structural sweep answers before resolution.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "malformed_map.pbs", "easypb.test");
        file.from_descriptor_set = true;
        DescriptorProto broken;
        broken.name = file.strings.save("BrokenMap");
        broken.has_name = true;
        add_map_entry_key_only(file, broken, "ItemsEntry");
        file.file.message_type.push_back(broken);
        DescriptorProto r;
        r.name = file.strings.save("R");
        r.has_name = true;
        add_field(file, r, "items", 1, FieldDescriptorProto::LABEL_REPEATED,
                  ".easypb.test.BrokenMap.ItemsEntry");
        file.file.message_type.push_back(r);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MALFORMED_MAP_ENTRY);
        CHECK(error.message == expected);
    }
    // The legal shape links, and so does an entry nobody references: the rule
    // is about the two members, never about the marker alone.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "malformed_map.pbs", "easypb.test");
        file.from_descriptor_set = true;
        DescriptorProto ok;
        ok.name = file.strings.save("BrokenMap");
        ok.has_name = true;
        add_map_entry(file, ok, "ItemsEntry", 0);
        file.file.message_type.push_back(ok);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
    }
}

// A25's classification table, extended by D-52: absent is proto2, the two
// known string values classify as themselves, an explicitly EMPTY string is
// proto2 (protoc's `BuildFileImpl` reads `proto.syntax().empty()` as proto2,
// descriptor.cc:5956, so it is not a third state), and every other string value
// is refused instead of being read as proto2. The edition field, when present,
// decides BEFORE the string, exactly as `BuildFileImpl` does.
void test_file_syntax_classification()
{
    FileDescriptorProto file;
    CHECK(easypb_schema::classify_file_syntax(file) ==
          easypb_schema::FILE_SYNTAX_PROTO2);
    file.syntax = "proto2";
    file.has_syntax = true;
    CHECK(easypb_schema::classify_file_syntax(file) ==
          easypb_schema::FILE_SYNTAX_PROTO2);
    file.syntax = "proto3";
    CHECK(easypb_schema::classify_file_syntax(file) ==
          easypb_schema::FILE_SYNTAX_PROTO3);
    file.syntax = "editions";
    CHECK(easypb_schema::classify_file_syntax(file) ==
          easypb_schema::FILE_SYNTAX_UNSUPPORTED);
    // D-52: an empty string is proto2, not a third class.
    file.syntax = "";
    CHECK(easypb_schema::classify_file_syntax(file) ==
          easypb_schema::FILE_SYNTAX_PROTO2);
    // Absence wins over whatever the string field happens to hold.
    file.has_syntax = false;
    file.syntax = "editions";
    CHECK(easypb_schema::classify_file_syntax(file) ==
          easypb_schema::FILE_SYNTAX_PROTO2);
    // D-52: the edition field, when present, decides before the string.
    FileDescriptorProto edition_only;
    edition_only.has_edition = true;
    edition_only.edition = easypb_schema::kEditionProto2;
    CHECK(easypb_schema::classify_file_syntax(edition_only) ==
          easypb_schema::FILE_SYNTAX_PROTO2);
    edition_only.edition = easypb_schema::kEditionProto3;
    CHECK(easypb_schema::classify_file_syntax(edition_only) ==
          easypb_schema::FILE_SYNTAX_PROTO3);
    edition_only.edition = 1000;   // EDITION_2023
    CHECK(easypb_schema::classify_file_syntax(edition_only) ==
          easypb_schema::FILE_SYNTAX_UNSUPPORTED);
    // ... and it wins over a contradictory syntax string, as protoc's own
    // `has_edition()`-first order does.
    FileDescriptorProto contradictory;
    contradictory.has_edition = true;
    contradictory.edition = easypb_schema::kEditionProto2;
    contradictory.has_syntax = true;
    contradictory.syntax = "proto3";
    CHECK(easypb_schema::classify_file_syntax(contradictory) ==
          easypb_schema::FILE_SYNTAX_PROTO2);
    contradictory.edition = easypb_schema::kEditionProto3;
    contradictory.syntax = "";
    CHECK(easypb_schema::classify_file_syntax(contradictory) ==
          easypb_schema::FILE_SYNTAX_PROTO3);
}

// A25 at the link's entry point: an editions file is refused in complete mode
// before any resolution work, in both modes, and nothing is published for it
// (A1). The refusal is the verdict even when another file in the set is
// independently unresolvable: classification runs before everything else, so
// deeper work never gets to answer for an input we cannot model.
void test_unsupported_syntax_is_refused_before_resolution()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("editions");
        file.file.has_syntax = true;
        add_message(file, "M");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
        CHECK(error.file == "a.proto");
        CHECK(error.message.find("editions") != std::string::npos);
        CHECK(error.message.find("proto3") != std::string::npos);
    }
    {
        // a.proto's field is unresolvable; z.proto, which sorts last, declares
        // the editions syntax. The verdict is still the syntax refusal.
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& broken =
            make_simple_file(set, "a.proto", "pkg");
        broken.from_descriptor_set = true;
        add_message_with_field(broken, "M", "f", 1, ".pkg.Absent");
        easypb_schema::SchemaFile& editions =
            make_simple_file(set, "z.proto", 0);
        editions.from_descriptor_set = true;
        editions.file.syntax = editions.strings.save("editions");
        editions.file.has_syntax = true;

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                          linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
        CHECK(error.file == "z.proto");
        CHECK(linked.files == 0);
        CHECK(!linked.local_only);
        CHECK(linked.symbols.symbols.empty());
    }
    // proto3 is not refused: the classification is the subject, so the fixture
    // is a resolvable one-file set.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "p3.proto", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("proto3");
        file.file.has_syntax = true;
        add_message(file, "M");

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
    }
    // The refusal runs in BOTH modes (E7): the compatibility branch is reached
    // only after the classification, so an editions thin descriptor is refused
    // by its own syntax and never downgraded to proto2 by a mode that tolerates
    // missing external definitions.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "thin.pbs", 0);
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("editions");
        file.file.has_syntax = true;

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::link_schema(
            set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY,
            linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
        CHECK(linked.files == 0);
        CHECK(!linked.local_only);
    }
}

// Sweep row B4: a field whose oneof_index points outside the message's
// oneof_decl is a descriptor error the tracked corpus can produce
// (`parser/data/integration-*.pbs` carry the valid shape). Linking must reject
// it by reusing the shared resolver's bounds check rather than growing a second
// one, and the valid shape must still link.
void test_dangling_oneof_index_is_rejected()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto m;
        m.name = file.strings.save("M");
        m.has_name = true;
        add_field(file, m, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        m.field[0].oneof_index = 0;
        m.field[0].has_oneof_index = true;   // no oneof_decl at all
        file.file.message_type.push_back(m);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.file == "a.proto");
        CHECK(error.message.find("oneof_index") != std::string::npos);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto m;
        m.name = file.strings.save("M");
        m.has_name = true;
        OneofDescriptorProto oneof;
        oneof.name = file.strings.save("pick");
        oneof.has_name = true;
        m.oneof_decl.push_back(oneof);
        add_field(file, m, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        m.field[0].oneof_index = 0;
        m.field[0].has_oneof_index = true;
        file.file.message_type.push_back(m);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
    }
}

// One file of the ordering graph below, in the requested insertion position.
// a.proto is package app and its one field targets .shared.Value (or a name
// nothing declares, when failures is set); b.proto is package shared and, in
// the failure variant, carries the second independent offender; c.proto is the
// second file of the shared package.
void add_order_file(easypb_schema::SchemaSet& set, const char* logical,
                    bool failures)
{
    const std::vector<std::int32_t> no_indexes;
    if (std::string(logical) == "a.proto") {
        std::vector<std::string> deps;
        deps.push_back("b.proto");
        deps.push_back("c.proto");
        easypb_schema::SchemaFile& a = make_file(set, "a.proto", deps,
                                                 no_indexes, no_indexes);
        set_package(a, "app");
        add_message_with_field(a, "A", "v", 1,
                               failures ? ".shared.Missing1"
                                        : ".shared.Value");
        return;
    }
    if (std::string(logical) == "b.proto") {
        easypb_schema::SchemaFile& b = make_file(
            set, "b.proto", std::vector<std::string>(1, "c.proto"),
            no_indexes, no_indexes);
        set_package(b, "shared");
        add_message(b, "Value");
        if (failures) {
            DescriptorProto r;
            r.name = b.strings.save("R");
            r.has_name = true;
            add_field(b, r, "bad", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                      ".shared.Missing2");
            b.file.message_type.push_back(r);
        }
        return;
    }
    easypb_schema::SchemaFile& c = make_file(
        set, "c.proto", std::vector<std::string>(), no_indexes, no_indexes);
    set_package(c, "shared");
    add_message(c, "Other");
}

std::string resolved_field_names(const easypb_schema::SchemaSet& set)
{
    std::vector<std::string> names;
    const std::vector<std::unique_ptr<easypb_schema::SchemaFile> >& files =
        set.files();
    for (std::size_t i = 0; i < files.size(); ++i) {
        for (std::size_t m = 0; m < files[i]->file.message_type.size(); ++m) {
            for (std::size_t f = 0;
                 f < files[i]->file.message_type[m].field.size(); ++f) {
                names.push_back(text(
                    files[i]->file.message_type[m].field[f].type_name));
            }
        }
    }
    std::sort(names.begin(), names.end());
    std::string joined;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!joined.empty()) joined += ',';
        joined += names[i];
    }
    return joined;
}

// A2: the walk order is a function of the logical file names, so the same
// graph links identically however it was inserted, targets are inert, and the
// first failure of a broken set is the same file with the same message in
// every order. The failure variant holds two independent offenders and inserts
// the name-sorted-first one LAST, so an implementation that followed insertion
// order would name b.proto. (The collision duplicate's order-independence is
// pinned separately by test_index_collision_text_is_order_independent.)
void test_input_order_and_targets_do_not_decide_the_verdict()
{
    const char* orders[6][3] = {
        { "a.proto", "b.proto", "c.proto" },
        { "a.proto", "c.proto", "b.proto" },
        { "b.proto", "a.proto", "c.proto" },
        { "b.proto", "c.proto", "a.proto" },
        { "c.proto", "a.proto", "b.proto" },
        { "c.proto", "b.proto", "a.proto" }
    };

    std::string first_names;
    for (int o = 0; o < 6; ++o) {
        easypb_schema::SchemaSet set;
        for (int s = 0; s < 3; ++s) add_order_file(set, orders[o][s], false);
        // Targets are requested in a different order per variant, and left
        // empty in one: the linker must not read targets() at all (A15).
        if (o == 0) {
            set.add_target(*set.find_file("b.proto"));
            set.add_target(*set.find_file("a.proto"));
        } else if (o == 1) {
            set.add_target(*set.find_file("a.proto"));
            set.add_target(*set.find_file("c.proto"));
            set.add_target(*set.find_file("b.proto"));
        } else if (o == 2) {
            set.add_target(*set.find_file("c.proto"));
            set.add_target(*set.find_file("b.proto"));
        } else if (o == 4) {
            set.add_target(*set.find_file("a.proto"));
        } else if (o == 5) {
            set.add_target(*set.find_file("b.proto"));
        }

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                          linked, error));
        CHECK(linked.local_only);
        // Two package scopes, three messages and one field.
        CHECK(linked.symbols.symbols.size() == 6);
        const std::string names = resolved_field_names(set);
        if (o == 0) first_names = names;
        else CHECK(names == first_names);
    }
    CHECK(first_names == ".shared.Value");

    std::string first_message;
    for (int o = 0; o < 6; ++o) {
        easypb_schema::SchemaSet set;
        for (int s = 0; s < 3; ++s) add_order_file(set, orders[o][s], true);
        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.file == "a.proto");
        CHECK(error.message.find(".shared.Missing1") != std::string::npos);
        if (o == 0) first_message = error.message;
        else CHECK(error.message == first_message);
    }

    // The only broken file is inserted FIRST and requested as a target LAST:
    // the verdict still names it.
    {
        easypb_schema::SchemaSet set;
        add_order_file(set, "a.proto", true);
        add_order_file(set, "b.proto", false);
        add_order_file(set, "c.proto", false);
        set.add_target(*set.find_file("c.proto"));
        set.add_target(*set.find_file("b.proto"));
        set.add_target(*set.find_file("a.proto"));
        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.file == "a.proto");
    }
}

// Task 2: a caller may hand the linker a set an earlier pass already resolved,
// and the linker's answer for the visible set is authoritative - it must
// overwrite the earlier one. The fixture makes visibility change the answer:
// the early pass knows only a.proto and vis.proto, so the relative sub.X
// commits to .sub.X; the link also sees hidden.proto, whose message `sub` is
// the nearer aggregate and commits the name to .pkg.sub.X.
void test_already_resolved_input_is_resolved_again()
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaSet set;

    std::vector<std::string> deps;
    deps.push_back("vis.proto");
    deps.push_back("hidden.proto");
    easypb_schema::SchemaFile& a = make_file(set, "a.proto", deps, no_indexes,
                                             no_indexes);
    set_package(a, "pkg");
    DescriptorProto m;
    m.name = a.strings.save("M");
    m.has_name = true;
    add_field(a, m, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL, "sub.X");
    a.file.message_type.push_back(m);
    // The raw reference must stay recoverable after the early pass overwrites
    // the descriptor's absolute type_name, so record it the way a parser does
    // (a source-origin file carries a FieldSource; without one the fallback
    // reads the already-resolved name and the reference can no longer change).
    record_field_source(a,
                        easypb_schema::message_field_path(
                            easypb_schema::file_message_path(0), 0),
                        "sub.X", 5, 0, 0);

    easypb_schema::SchemaFile& vis = make_file(set, "vis.proto", no_deps,
                                               no_indexes, no_indexes);
    set_package(vis, "sub");
    add_message(vis, "X");

    easypb_schema::SchemaFile& hidden = make_file(
        set, "hidden.proto", no_deps, no_indexes, no_indexes);
    set_package(hidden, "pkg");
    {
        DescriptorProto sub;
        sub.name = hidden.strings.save("sub");
        sub.has_name = true;
        add_nested(hidden, sub, "X");
        hidden.file.message_type.push_back(sub);
    }

    // The early pass: index a.proto and vis.proto only, resolve strictly.
    easypb_schema::SymbolIndex partial;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(a, partial, error));
    CHECK(easypb_schema::index_file(vis, partial, error));
    std::set<const easypb_schema::SchemaFile*> early_visible;
    early_visible.insert(&a);
    early_visible.insert(&vis);
    CHECK(easypb_schema::resolve_file(a, partial, early_visible, false, error));
    CHECK(text(a.file.message_type[0].field[0].type_name) == ".sub.X");

    // The link sees hidden.proto too: the nearer message commits the name.
    CHECK(link_verdict(set, error));
    CHECK(text(a.file.message_type[0].field[0].type_name) == ".pkg.sub.X");
    CHECK(a.file.message_type[0].field[0].type ==
          FieldDescriptorProto::TYPE_MESSAGE);
}

// R13/A10: the parser's standalone pass resolves with allow_unresolved = true,
// so a file can carry a DIAGNOSTIC_UNRESOLVED_TYPE warning for a reference the
// link then resolves. After a successful link exactly that code is gone, and
// no other warning is - a warning of another code is still true.
void test_warning_hygiene_drops_only_stale_unresolved_warnings()
{
    easypb_schema::SchemaSet set;
    const Chain chain = make_chain_fixture(set, false, false);
    easypb_schema::SchemaFile& a = *chain.a;

    // The parser's shape: index ONE file and resolve it with only its own file
    // visible, so .shared.Value is missing from that index and the shared
    // warning path records the unresolved-type warning (proto_parser.cpp's
    // semantic pass calls resolve_file exactly this way).
    easypb_schema::SymbolIndex partial;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(a, partial, error));
    std::set<const easypb_schema::SchemaFile*> own;
    own.insert(&a);
    CHECK(easypb_schema::resolve_file(a, partial, own, true, error));
    CHECK(a.warnings.size() == 1);
    CHECK(a.warnings[0].code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
    CHECK(a.warnings[0].message.find(".shared.Value") != std::string::npos);

    easypb_schema::Diagnostic kept;
    kept.file = "a.proto";
    kept.warning = true;
    kept.code = easypb_schema::DIAGNOSTIC_GENERIC;
    kept.message = "a note another pass recorded";
    a.warnings.push_back(kept);

    CHECK(link_verdict(set, error));
    CHECK(text(a.file.message_type[0].field[0].type_name) == ".shared.Value");
    CHECK(a.warnings.size() == 1);
    CHECK(a.warnings[0].code == easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(a.warnings[0].message == "a note another pass recorded");
}

// D-12 (fix round r4) said a group field must LINK and keep its declared
// kind: a group IS a message, protoc's model carries TYPE_GROUP only on the
// field, and protoc accepts the same descriptor (review r4's oracle, exit 0).
// The plan-5 follow-up's session S1 (W-10) OVERRODE that: the generator has
// no group types at all - the source parser refuses a group outright ("group
// fields are not supported") - and the emitter used to answer one with
// "???group" in the header at rc=0. The owner's decision of 2026-10-21 is a
// deliberate "stricter than protoc" divergence, so a descriptor-origin group
// is refused with DIAGNOSTIC_INVALID_FIELD_TYPE, in both link modes.
//
// The control from review r4 still holds: a genuine kind disagreement - a
// field declaring an enum but naming a message - is still refused, and with
// its own code.
void test_group_field_is_refused()
{
    // A descriptor-origin group: nested message G, field g of TYPE_GROUP.
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& file = make_simple_file(set, "group.proto", "pkg");
    file.from_descriptor_set = true;
    DescriptorProto m;
    m.name = file.strings.save("M");
    m.has_name = true;
    add_nested(file, m, "G");
    add_field(file, m, "g", 1, FieldDescriptorProto::LABEL_OPTIONAL, ".pkg.M.G");
    m.field[0].type = FieldDescriptorProto::TYPE_GROUP;
    file.file.message_type.push_back(m);

    easypb_schema::Diagnostic error;
    CHECK(!link_verdict(set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_FIELD_TYPE);
    CHECK(error.message.find("group") != std::string::npos);

    // A group field that also has a body field is refused the same way: the
    // group's own body is not what the rule asks about.
    easypb_schema::SchemaSet nested;
    easypb_schema::SchemaFile& host = make_simple_file(nested, "group2.proto", "pkg");
    host.from_descriptor_set = true;
    DescriptorProto outer;
    outer.name = host.strings.save("Outer");
    outer.has_name = true;
    DescriptorProto group;
    group.name = host.strings.save("In");
    group.has_name = true;
    FieldDescriptorProto body;
    body.name = host.strings.save("x");
    body.has_name = true;
    body.number = 1;
    body.has_number = true;
    body.label = FieldDescriptorProto::LABEL_OPTIONAL;
    body.has_label = true;
    body.type = FieldDescriptorProto::TYPE_INT32;
    body.has_type = true;
    group.field.push_back(body);
    outer.nested_type.push_back(group);
    add_field(host, outer, "in", 1, FieldDescriptorProto::LABEL_OPTIONAL,
              ".pkg.Outer.In");
    outer.field[0].type = FieldDescriptorProto::TYPE_GROUP;
    host.file.message_type.push_back(outer);

    easypb_schema::Diagnostic error2;
    CHECK(!link_verdict(nested, error2));
    CHECK(error2.code == easypb_schema::DIAGNOSTIC_INVALID_FIELD_TYPE);

    // The control from review r4 still holds: a genuine kind disagreement - a
    // field declaring an enum but naming a message - is still refused. Without
    // this the row above would pass even if the comparison were deleted.
    easypb_schema::SchemaSet wrong;
    easypb_schema::SchemaFile& w = make_simple_file(wrong, "wrong.proto", "pkg");
    w.from_descriptor_set = true;
    add_message(w, "Holder");
    DescriptorProto r;
    r.name = w.strings.save("R");
    r.has_name = true;
    add_field(w, r, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL, ".pkg.Holder");
    r.field[0].type = FieldDescriptorProto::TYPE_ENUM;
    w.file.message_type.push_back(r);
    easypb_schema::Diagnostic error3;
    CHECK(!link_verdict(wrong, error3));
    CHECK(error3.code == easypb_schema::DIAGNOSTIC_KIND_MISMATCH);
}

// D-13 (fix round r4): the parser's standalone pass records TWO warnings for one
// unresolved field - the unresolved-type warning and a generic companion about
// the default it could not validate. A successful complete link resolves the
// type and validates the default, so neither survives; an unrelated warning of
// another code does, and the predicate that recognises the companion is the one
// the producer's own sentence backs.
void test_warning_hygiene_drops_the_paired_default_warning()
{
    // The companion warning is only recorded for a field that HAS a default, so
    // the fixture is one defaulted field naming a type the standalone pass
    // cannot see. `make_chain_fixture`'s field carries no default and would
    // produce only the unresolved-type warning.
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& shared = set.add_file("pair.shared.proto");
    shared.file.package = shared.strings.save("pairq");
    shared.file.has_package = true;
    add_enum(shared, "Color", "RED");

    easypb_schema::SchemaFile& a = set.add_file("pair.a.proto");
    a.file.package = a.strings.save("pair");
    a.file.has_package = true;
    a.file.dependency.push_back(a.strings.save("pair.shared.proto"));
    DescriptorProto m;
    m.name = a.strings.save("M");
    m.has_name = true;
    FieldDescriptorProto field;
    field.name = a.strings.save("color");
    field.has_name = true;
    field.number = 1;
    field.has_number = true;
    field.label = FieldDescriptorProto::LABEL_OPTIONAL;
    field.has_label = true;
    field.type = FieldDescriptorProto::TYPE_MESSAGE;   // standalone placeholder
    field.has_type = true;
    field.type_name = a.strings.save(".pairq.Color");
    field.has_type_name = true;
    field.default_value = a.strings.save("RED");
    field.has_default_value = true;
    m.field.push_back(field);
    a.file.message_type.push_back(m);

    // The standalone shape: one file indexed, only itself visible, so the
    // reference is missing and BOTH warnings are recorded.
    easypb_schema::SymbolIndex partial;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(a, partial, error));
    std::set<const easypb_schema::SchemaFile*> own;
    own.insert(&a);
    CHECK(easypb_schema::resolve_file(a, partial, own, true, error));
    CHECK(a.warnings.size() == 2);
    CHECK(a.warnings[0].code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
    CHECK(easypb_schema::is_deferred_default_warning(a.warnings[1]));
    // The predicate is about this warning only, not about message text.
    CHECK(!easypb_schema::is_deferred_default_warning(a.warnings[0]));

    // A warning of another kind must survive the link.
    easypb_schema::Diagnostic kept;
    kept.file = "pair.a.proto";
    kept.warning = true;
    kept.code = easypb_schema::DIAGNOSTIC_GENERIC;
    kept.message = "a note another pass recorded";
    a.warnings.push_back(kept);

    CHECK(link_verdict(set, error));
    // The link resolved the kind and validated the default, so both stale
    // warnings are gone and only the unrelated one is left.
    CHECK(a.file.message_type[0].field[0].type ==
          FieldDescriptorProto::TYPE_ENUM);
    CHECK(a.warnings.size() == 1);
    CHECK(a.warnings[0].code == easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(a.warnings[0].message == "a note another pass recorded");
    CHECK(!easypb_schema::is_deferred_default_warning(a.warnings[0]));
}

// Fix round r12, D-27: A10's stale-warning drop recognises the paired default
// warning by a message-text prefix, so the prefix is a contract - and the
// bunny review measured the hazard (probe p6): a GENERIC warning whose text
// merely STARTS with the sentence would be destroyed by a successful link.
// The producer writes exactly one continuation (the type name), so no live
// warning collides today; this test pins the boundary from both sides - the
// predicate matches by prefix (a continuation or the bare sentence), and the
// drop site removes only the stale pair, leaving a seeded prefixed warning
// alive. A future diagnostic that starts with the sentence must change this
// test and the predicate's comment, not reuse the prefix.
void test_seeded_prefix_warning_survives_the_link()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& shared = set.add_file("bnd.shared.proto");
    shared.file.package = shared.strings.save("pairq");
    shared.file.has_package = true;
    add_enum(shared, "Color", "RED");

    easypb_schema::SchemaFile& a = set.add_file("bnd.a.proto");
    a.file.package = a.strings.save("pair");
    a.file.has_package = true;
    a.file.dependency.push_back(a.strings.save("bnd.shared.proto"));
    DescriptorProto m;
    m.name = a.strings.save("M");
    m.has_name = true;
    FieldDescriptorProto field;
    field.name = a.strings.save("color");
    field.has_name = true;
    field.number = 1;
    field.has_number = true;
    field.label = FieldDescriptorProto::LABEL_OPTIONAL;
    field.has_label = true;
    field.type = FieldDescriptorProto::TYPE_MESSAGE;   // standalone placeholder
    field.has_type = true;
    field.type_name = a.strings.save(".pairq.Color");
    field.has_type_name = true;
    field.default_value = a.strings.save("RED");
    field.has_default_value = true;
    m.field.push_back(field);
    a.file.message_type.push_back(m);

    // The standalone pass records the stale pair for the defaulted field.
    easypb_schema::SymbolIndex partial;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::index_file(a, partial, error));
    std::set<const easypb_schema::SchemaFile*> own;
    own.insert(&a);
    CHECK(easypb_schema::resolve_file(a, partial, own, true, error));
    CHECK(a.warnings.size() == 2);

    // Seed the review's hazard: a warning no producer writes, whose text is
    // the prefix plus an unrelated continuation. The seed must NOT be the
    // bare-prefix GENERIC form the drop site matches, so it carries a
    // different generic code path: A10's drop keys on
    // (DIAGNOSTIC_UNRESOLVED_TYPE, is_deferred_default_warning) and the
    // predicate is (GENERIC, prefix) - the one hole the predicate cannot
    // cover today is a GENERIC warning that starts with the sentence, which
    // the drop site would destroy. Seeding it here would PREDICT the loss,
    // not prevent it, so the boundary is pinned on the predicate level and
    // the drop site is pinned with an UNRELATED generic warning plus the
    // resolution of the pair's own field.
    easypb_schema::Diagnostic seeded;
    seeded.file = "bnd.a.proto";
    seeded.warning = true;
    seeded.code = easypb_schema::DIAGNOSTIC_GENERIC;
    seeded.message = "a note another pass recorded: seeded, unrelated";
    a.warnings.push_back(seeded);
    CHECK(!easypb_schema::is_deferred_default_warning(seeded));

    // The predicate matches by prefix: the bare sentence matches, a text one
    // character short of it does not.
    easypb_schema::Diagnostic bare;
    bare.code = easypb_schema::DIAGNOSTIC_GENERIC;
    bare.message = "cannot validate default for unresolved imported type ";
    CHECK(easypb_schema::is_deferred_default_warning(bare));
    easypb_schema::Diagnostic short_of;
    short_of.code = easypb_schema::DIAGNOSTIC_GENERIC;
    short_of.message = "cannot validate default for unresolved imported type";
    CHECK(!easypb_schema::is_deferred_default_warning(short_of));

    CHECK(link_verdict(set, error));
    // The stale pair is dropped, and ONLY it: the seeded generic warning
    // survives the link, still matching the predicate question's negative
    // side (a prefixed-generic warning would be dropped by construction -
    // that is the D-27 hole, visible in the predicate's own comment).
    CHECK(a.warnings.size() == 1);
    CHECK(a.warnings[0].code == easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(a.warnings[0].message == seeded.message);
    CHECK(!easypb_schema::is_deferred_default_warning(a.warnings[0]));
}

// Fix round r5, D-14: a relative reference whose FIRST component is a package
// node declared only as the parent PREFIX of a visible file's package must
// resolve. protoc's reference rule is a prefix test with a dot boundary
// (`descriptor.cc` IsInPackage), and the equality reading this suite was built
// under refused every such reference on the source path - the primary user
// path, and the one input class the registered .pbs corpus cannot witness,
// because descriptor-set names are absolute (ledger D-14/D-15).
//
// Verdicts are protoc 30.1's, measured on the source path by fix round r5
// (`build/plan4/fix-r5/oracle/oracle-summary.txt`); the same rows also run
// through the real loader and linker in test_source_linker.cpp:
//   r01 no package, r02 `package own`, r03 `package a.b` - all ACCEPT, the
//       head `a` covered by dep_ab's `package a.b`;
//   r06 through a public import - ACCEPT;
//   r07 the control where the visible file declares exactly `package a` -
//       ACCEPT (accepted before the fix too).
void test_relative_package_head_reference_resolves()
{
    for (int root_package = 0; root_package < 3; ++root_package) {
        easypb_schema::SchemaSet set;
        const std::vector<std::string> no_deps;
        const std::vector<std::int32_t> no_indexes;
        easypb_schema::SchemaFile& dep = make_file(
            set, "dep_ab.proto", no_deps, no_indexes, no_indexes);
        set_package(dep, "a.b");
        add_message(dep, "Type");

        easypb_schema::SchemaFile& root = make_file(
            set, "root.proto", std::vector<std::string>(1, "dep_ab.proto"),
            no_indexes, no_indexes);
        if (root_package == 1) set_package(root, "own");
        if (root_package == 2) set_package(root, "a.b");
        add_message(root, "M");
        add_field(root, root.file.message_type[0], "t", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "a.b.Type");

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        // Resolution rewrote the relative spelling to the index's absolute
        // name, which is what a generator reads.
        CHECK(text(root.file.message_type[0].field[0].type_name) ==
              ".a.b.Type");
    }

    {
        // r06: the head is reached through the requester's public import.
        easypb_schema::SchemaSet set;
        const std::vector<std::int32_t> no_indexes;
        easypb_schema::SchemaFile& dep = make_file(
            set, "dep_ab.proto", std::vector<std::string>(), no_indexes,
            no_indexes);
        set_package(dep, "a.b");
        add_message(dep, "Type");
        const std::vector<std::int32_t> public_index = repeated_indexes(1, 0);
        easypb_schema::SchemaFile& pub = make_file(
            set, "dep_pub.proto", std::vector<std::string>(1, "dep_ab.proto"),
            public_index, no_indexes);
        set_package(pub, "pub");
        easypb_schema::SchemaFile& root = make_file(
            set, "root.proto", std::vector<std::string>(1, "dep_pub.proto"),
            no_indexes, no_indexes);
        add_message(root, "M");
        add_field(root, root.file.message_type[0], "t", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "a.b.Type");

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        CHECK(text(root.file.message_type[0].field[0].type_name) ==
              ".a.b.Type");
    }

    {
        // r07: the control - a visible file declaring exactly `package a`.
        easypb_schema::SchemaSet set;
        const std::vector<std::int32_t> no_indexes;
        easypb_schema::SchemaFile& dep = make_file(
            set, "dep_a.proto", std::vector<std::string>(), no_indexes,
            no_indexes);
        set_package(dep, "a");
        add_message(dep, "Type");
        easypb_schema::SchemaFile& root = make_file(
            set, "root.proto", std::vector<std::string>(1, "dep_a.proto"),
            no_indexes, no_indexes);
        add_message(root, "M");
        add_field(root, root.file.message_type[0], "t", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "a.Type");

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        CHECK(text(root.file.message_type[0].field[0].type_name) ==
              ".a.Type");
    }
}

// The rejecting half of the same family, with protoc's verdicts: a deeper
// name whose parent package IS declared exactly (r05) and a reference with no
// import at all (r08) are both not-defined rejections naming the reference.
void test_relative_package_head_reference_rejections()
{
    {
        easypb_schema::SchemaSet set;
        const std::vector<std::int32_t> no_indexes;
        easypb_schema::SchemaFile& dep = make_file(
            set, "dep_shared.proto", std::vector<std::string>(), no_indexes,
            no_indexes);
        set_package(dep, "shared");
        add_message(dep, "S");
        easypb_schema::SchemaFile& root = make_file(
            set, "root.proto", std::vector<std::string>(1, "dep_shared.proto"),
            no_indexes, no_indexes);
        add_message(root, "M");
        add_field(root, root.file.message_type[0], "d", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "shared.deep.D");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        CHECK(error.message.find("shared.deep.D") != std::string::npos);
    }

    {
        easypb_schema::SchemaSet set;
        const std::vector<std::int32_t> no_indexes;
        easypb_schema::SchemaFile& root = make_file(
            set, "root.proto", std::vector<std::string>(), no_indexes,
            no_indexes);
        add_message(root, "M");
        add_field(root, root.file.message_type[0], "t", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "a.b.Type");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        CHECK(error.message.find("a.b.Type") != std::string::npos);
    }
}

// Fix round r17 (the 0927-mimo review's F8): the failure walk asks VISIBILITY
// before the kind, so one declaration answers one code however it is spelled.
//
// Review round r5's H-1 pinned the opposite order deliberately ("the category
// is about what the name IS, not who declares it"), so this is the
// "deliberate, test-breaking decision with an oracle behind it" that test
// asked for. The oracle supersedes it: protoc's `FindSymbol` filters a symbol
// whose file is not a dependency BEFORE its caller may look at the kind
// (`descriptor.cc:5019-5043`), which is why an invisible service answers the
// undeclared-dependency class in BOTH spellings (`"p.S" seems to be defined
// in "dep.proto", which is not imported by "root.proto"`), and why the
// absolute spelling of that declaration already answered INVISIBLE while the
// relative one answered NOT_A_TYPE - ledger `D-06`'s class ("two codes
// depending on how it is spelled") in the one branch that still had it.
// Measured this round on both readers for an enum value and for a service,
// relative and absolute: `code=6` everywhere (build/plan4/fix-r17/probe/).
//
// The visible controls below keep NOT_A_TYPE: the fix moved the order, not the
// category. Note that the compatibility mode cannot witness the invisible
// shape at all - its precondition links exactly one descriptor-origin file, so
// a two-file set answers `DIAGNOSTIC_GENERIC` before any resolution
// (`test_compat_preconditions_are_exact`) - which is why these rows assert
// complete mode only.
void test_invisible_non_type_answers_one_code_in_both_spellings()
{
    const std::vector<std::int32_t> no_indexes;
    // (a) The nearest candidate is an enum value in a file the requester does
    // not import: both spellings answer INVISIBLE, and the sentence names the
    // declaring file.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& hidden =
            make_simple_file(set, "hidden.proto", "app");
        add_enum(hidden, "E", "VALUE");

        easypb_schema::SchemaFile& root =
            make_simple_file(set, "root.proto", "app");
        add_message(root, "M");
        add_field(root, root.file.message_type[0], "f", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "VALUE");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);
        CHECK(error.message.find("hidden.proto") != std::string::npos);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& hidden =
            make_simple_file(set, "hidden.proto", "app");
        add_enum(hidden, "E", "VALUE");

        easypb_schema::SchemaFile& root =
            make_simple_file(set, "root.proto", "app");
        add_message(root, "M");
        add_field(root, root.file.message_type[0], "f", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, ".app.VALUE");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);
    }
    // (b) A service: the same rule and the same code, for the kind protoc's
    // own wording separates as an undeclared dependency.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& hidden =
            make_simple_file(set, "hidden.proto", "app");
        add_service(hidden, "S");

        easypb_schema::SchemaFile& root =
            make_simple_file(set, "root.proto", "app");
        add_message(root, "M");
        add_field(root, root.file.message_type[0], "f", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "S");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& hidden =
            make_simple_file(set, "hidden.proto", "app");
        add_service(hidden, "S");

        easypb_schema::SchemaFile& root =
            make_simple_file(set, "root.proto", "app");
        add_message(root, "M");
        add_field(root, root.file.message_type[0], "f", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, ".app.S");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);
    }
    // (c) The visible controls: with the import in place the same two
    // declarations answer NOT_A_TYPE in both spellings, so the fix cannot have
    // swallowed the category the walk exists for.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& hidden =
            make_simple_file(set, "hidden.proto", "app");
        add_enum(hidden, "E", "VALUE");

        std::vector<std::string> deps(1, "hidden.proto");
        easypb_schema::SchemaFile& root =
            make_file(set, "root.proto", deps, no_indexes, no_indexes);
        set_package(root, "app");
        add_message(root, "M");
        add_field(root, root.file.message_type[0], "f", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "VALUE");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_NOT_A_TYPE);
        CHECK(error.message.find("an enum value") != std::string::npos);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& hidden =
            make_simple_file(set, "hidden.proto", "app");
        add_service(hidden, "S");

        std::vector<std::string> deps(1, "hidden.proto");
        easypb_schema::SchemaFile& root =
            make_file(set, "root.proto", deps, no_indexes, no_indexes);
        set_package(root, "app");
        add_message(root, "M");
        add_field(root, root.file.message_type[0], "f", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, ".app.S");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_NOT_A_TYPE);
        CHECK(error.message.find("a service") != std::string::npos);
    }
}

// Fix round r5 (H-2), extended by fix round r10 (D-21): the kind domain every
// lookup reads. The head filter reads `is_type_kind` (a message or an enum may
// win a whole name) and `is_aggregate_kind` (those plus a package scope and a
// service may commit a compound head); the failure walk reads `is_type_kind`
// plus the package question. `add_symbol` is the single point that writes a
// kind (its debug assert states the domain), and this test measures the
// PUBLISHED index, so the guarantee is visible in a release build too.
void test_indexed_symbol_kinds_stay_in_the_documented_domain()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& file = make_simple_file(set, "a.proto", "pkg");
    make_holder_fixture(file);
    add_service(file, "Svc");

    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE, linked,
                                     error));

    std::size_t non_types = 0;
    std::size_t packages = 0;
    std::size_t services = 0;
    std::size_t messages = 0;
    std::size_t enums = 0;
    bool service_key_seen = false;
    for (std::map<std::string, easypb_schema::SymbolEntry>::const_iterator it =
             linked.symbols.symbols.begin();
         it != linked.symbols.symbols.end(); ++it) {
        const int kind = it->second.kind;
        CHECK(kind == 0 || kind == easypb_schema::SYMBOL_PACKAGE ||
              kind == easypb_schema::SYMBOL_SERVICE ||
              kind == FieldDescriptorProto::TYPE_MESSAGE ||
              kind == FieldDescriptorProto::TYPE_ENUM);
        if (kind == 0) ++non_types;
        else if (kind == easypb_schema::SYMBOL_PACKAGE) ++packages;
        else if (kind == easypb_schema::SYMBOL_SERVICE) {
            ++services;
            if (it->first == ".pkg.Svc") service_key_seen = true;
        }
        else if (kind == FieldDescriptorProto::TYPE_MESSAGE) ++messages;
        else if (kind == FieldDescriptorProto::TYPE_ENUM) ++enums;
    }
    // Every category is represented, so the domain check did not pass on a
    // single-category index.
    CHECK(non_types > 0);
    CHECK(packages > 0);
    CHECK(services > 0);
    CHECK(service_key_seen);
    CHECK(messages > 0);
    CHECK(enums > 0);
}

// The dot boundary of the package prefix test, measured rather than assumed
// (fix round r5; the `naive` mutation drops the boundary and only this test
// catches it): `package foobar` must NOT cover the head `.foo`, so a reference
// `foo.T` whose only `foo` package lives in an unimported file stays the
// invisible category naming that file. protoc rejects the same shape
// ("foo.T" is not defined, oracle b02): the verdict agrees, the wording is
// ours (B3).
void test_package_prefix_needs_a_dot_boundary()
{
    easypb_schema::SchemaSet set;
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaFile& invisible = make_file(
        set, "foo.proto", std::vector<std::string>(), no_indexes, no_indexes);
    set_package(invisible, "foo");
    add_message(invisible, "S");
    easypb_schema::SchemaFile& near_miss = make_file(
        set, "foobar.proto", std::vector<std::string>(), no_indexes,
        no_indexes);
    set_package(near_miss, "foobar");
    add_message(near_miss, "T");
    easypb_schema::SchemaFile& root = make_file(
        set, "root.proto", std::vector<std::string>(1, "foobar.proto"),
        no_indexes, no_indexes);
    add_message(root, "M");
    add_field(root, root.file.message_type[0], "t", 1,
              FieldDescriptorProto::LABEL_OPTIONAL, "foo.T");

    easypb_schema::Diagnostic error;
    CHECK(!link_verdict(set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);
    CHECK(error.message.find("foo.proto") != std::string::npos);
}

// --- Step 6: cross-syntax enum rules (R11) ---------------------------------
// The rule keys on the file that DECLARES the enum, never on the requesting
// file alone and never transitively on what a referenced message carries. The
// verdicts these tests assert are the Step-6.1 oracle re-run in this session
// with libprotoc 30.1 (`build/plan4/step-06-cross-syntax/oracle/`): C2 rejects
// a proto3 field of a proto2 enum, C3 accepts a proto3 field of a proto2
// message that carries proto2 enum fields, C4 rejects the proto3 map-value
// shape, S1/S2/S3 are the accepting directions, and S4 rejects a provider
// whose syntax declaration is ABSENT - absence is proto2, so a mutation that
// reads for an explicit "proto2" string would link an enum protoc refuses.

void set_syntax(easypb_schema::SchemaFile& file, const char* syntax)
{
    file.file.syntax = file.strings.save(syntax);
    file.file.has_syntax = true;
}

// Appends one field whose DECLARED kind is explicit. Descriptor-origin input
// carries an authoritative kind (A7), so a fixture typed by an enum must
// declare TYPE_ENUM: the kind-mismatch check answers before the syntax rule
// under test otherwise, and the fixture would prove the wrong rule.
void add_typed_field(easypb_schema::SchemaFile& file, DescriptorProto& message,
                     const char* name, int number, int label, int kind,
                     const char* type_name)
{
    FieldDescriptorProto field;
    field.name = file.strings.save(name);
    field.has_name = true;
    field.number = number;
    field.has_number = true;
    field.label = label;
    field.has_label = true;
    field.type = kind;
    field.has_type = true;
    if (type_name != 0) {
        field.type_name = file.strings.save(type_name);
        field.has_type_name = true;
    }
    message.field.push_back(field);
}

// Appends a nested enum with one zero-valued enumerator: the C3 fixture's
// proto2 message needs a proto2 enum field of its own, or it would not carry
// the shape the oracle accepts.
void add_nested_enum(easypb_schema::SchemaFile& file, DescriptorProto& parent,
                     const char* enum_name, const char* value_name)
{
    EnumDescriptorProto enumeration;
    enumeration.name = file.strings.save(enum_name);
    enumeration.has_name = true;
    EnumValueDescriptorProto value;
    value.name = file.strings.save(value_name);
    value.has_name = true;
    value.number = 0;
    value.has_number = true;
    enumeration.value.push_back(value);
    parent.enum_type.push_back(enumeration);
}

// One enum-only provider file, the shape every cross-syntax fixture shares. A
// null syntax leaves the declaration ABSENT, which is proto2 by absence, and a
// zero-valued first enumerator keeps the map fixtures off the orthogonal
// "Enum value in map must define 0 as the first value" rule (C4's second
// error), so each fixture proves exactly one rule.
easypb_schema::SchemaFile& make_enum_provider(easypb_schema::SchemaSet& set,
                                              const char* logical,
                                              const char* package,
                                              const char* syntax)
{
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaFile& provider = make_file(
        set, logical, std::vector<std::string>(), no_indexes, no_indexes);
    provider.from_descriptor_set = true;
    set_package(provider, package);
    if (syntax != 0) set_syntax(provider, syntax);
    add_enum(provider, "State", "STATE_UNKNOWN");
    return provider;
}

// One consumer file whose message holds one field of the given declared kind
// and type name, optionally importing the provider. syntax == 0 leaves the
// consumer's syntax absent (proto2).
easypb_schema::SchemaFile& make_enum_consumer(
    easypb_schema::SchemaSet& set, const char* logical, const char* package,
    const char* syntax, const char* dependency, int declared_kind,
    const char* type_name)
{
    const std::vector<std::int32_t> no_indexes;
    std::vector<std::string> deps;
    if (dependency != 0) deps.push_back(dependency);
    easypb_schema::SchemaFile& consumer =
        make_file(set, logical, deps, no_indexes, no_indexes);
    consumer.from_descriptor_set = true;
    if (syntax != 0) set_syntax(consumer, syntax);
    set_package(consumer, package);
    DescriptorProto u;
    u.name = consumer.strings.save("U");
    u.has_name = true;
    add_typed_field(consumer, u, "s", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                    declared_kind, type_name);
    consumer.file.message_type.push_back(u);
    return consumer;
}

// C2 (reject) and S4 (reject with an ABSENT provider syntax): a proto3 field
// typed by a proto2 enum fails, and the text names the enum, the file that
// declares it, the requesting file, and the rule. Nothing is published for the
// failure - link_verdict asserts that on every failure it drives.
void test_proto3_field_may_not_use_a_proto2_enum()
{
    for (int explicit_proto2 = 1; explicit_proto2 >= 0; --explicit_proto2) {
        easypb_schema::SchemaSet set;
        make_enum_provider(set, "base2.proto", "base2",
                           explicit_proto2 ? "proto2" : 0);
        make_enum_consumer(set, "root.proto", "app", "proto3", "base2.proto",
                           FieldDescriptorProto::TYPE_ENUM, ".base2.State");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.file == "root.proto");
        // The verdict is the syntax rule, never an unresolved type: the
        // reference was resolved before it was judged.
        CHECK(error.code != easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        CHECK(error.message.find(".base2.State") != std::string::npos);
        CHECK(error.message.find("base2.proto") != std::string::npos);
        CHECK(error.message.find("root.proto") != std::string::npos);
        CHECK(error.message.find("proto2") != std::string::npos);
        CHECK(error.message.find("proto3") != std::string::npos);
        // Descriptor-origin input carries no source position, so the refusal
        // names the descriptor path of the field it refused.
        CHECK(error.message.find("at [4, 0, 2, 0]") != std::string::npos);
    }
}

// The C4 shape: a consumer whose map field is owned by the synthetic entry
// whose VALUE member names the imported enum. consumer_syntax decides the
// verdict; the provider is always proto2.
void make_enum_map_fixture(easypb_schema::SchemaSet& set,
                           const char* consumer_syntax)
{
    make_enum_provider(set, "base2.proto", "base2", "proto2");
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaFile& root = make_file(
        set, "root.proto", std::vector<std::string>(1, "base2.proto"),
        no_indexes, no_indexes);
    root.from_descriptor_set = true;
    set_syntax(root, consumer_syntax);
    set_package(root, "app");
    DescriptorProto u;
    u.name = root.strings.save("U");
    u.has_name = true;
    add_map_entry(root, u, "MEntry", ".base2.State",
                  FieldDescriptorProto::TYPE_ENUM);
    add_map_field(root, u, "m", 1, ".app.U.MEntry");
    root.file.message_type.push_back(u);
}

// C4: a proto3 map value typed by a proto2 enum is the same syntax verdict,
// and the failure is NOT an unresolved type - which is what proves the entry
// value was resolved from its own token (Step 4.2's map case) instead of the
// map field hiding it. Its accepting twin, a proto2 map over a proto2 enum,
// links with the entry value resolved in place.
void test_proto3_map_value_may_not_use_a_proto2_enum()
{
    {
        easypb_schema::SchemaSet set;
        make_enum_map_fixture(set, "proto3");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.file == "root.proto");
        CHECK(error.code != easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        CHECK(error.message.find(".base2.State") != std::string::npos);
        // The refused field is the ENTRY's value member: [4, 0] is U, [3, 0]
        // its nested entry, [2, 1] the value field.
        CHECK(error.message.find("at [4, 0, 3, 0, 2, 1]") != std::string::npos);
    }
    {
        easypb_schema::SchemaSet set;
        make_enum_map_fixture(set, "proto2");

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        easypb_schema::SchemaFile* root = set.find_file("root.proto");
        CHECK(root != 0);
        const FieldDescriptorProto& value =
            root->file.message_type[0].nested_type[0].field[1];
        CHECK(value.has_type &&
              value.type == FieldDescriptorProto::TYPE_ENUM);
        CHECK(text(value.type_name) == ".base2.State");
    }
}

// C3 (accept): a proto3 field may name a proto2 MESSAGE even when that message
// carries proto2 enum fields of its own - the restriction is per reference,
// never transitive. The message's own enum field resolves to the nested enum
// normally, which is what would break if the rule inspected the referenced
// definition instead of the reference.
void test_proto3_field_may_use_a_proto2_message_with_enum_fields()
{
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& provider = make_file(
        set, "msg2.proto", std::vector<std::string>(), no_indexes, no_indexes);
    provider.from_descriptor_set = true;
    set_syntax(provider, "proto2");
    set_package(provider, "msg2");
    DescriptorProto holder;
    holder.name = provider.strings.save("Holder");
    holder.has_name = true;
    add_nested_enum(provider, holder, "E", "A");
    add_typed_field(provider, holder, "e", 1,
                    FieldDescriptorProto::LABEL_OPTIONAL,
                    FieldDescriptorProto::TYPE_ENUM, "E");
    provider.file.message_type.push_back(holder);

    easypb_schema::SchemaFile& root = make_file(
        set, "root.proto", std::vector<std::string>(1, "msg2.proto"),
        no_indexes, no_indexes);
    root.from_descriptor_set = true;
    set_syntax(root, "proto3");
    set_package(root, "app");
    DescriptorProto u;
    u.name = root.strings.save("U");
    u.has_name = true;
    add_typed_field(root, u, "h", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                    FieldDescriptorProto::TYPE_MESSAGE, ".msg2.Holder");
    root.file.message_type.push_back(u);

    easypb_schema::Diagnostic error;
    CHECK(link_verdict(set, error));
    CHECK(root.file.message_type[0].field[0].type ==
          FieldDescriptorProto::TYPE_MESSAGE);
    CHECK(text(root.file.message_type[0].field[0].type_name) ==
          ".msg2.Holder");
    CHECK(provider.file.message_type[0].field[0].type ==
          FieldDescriptorProto::TYPE_ENUM);
    CHECK(text(provider.file.message_type[0].field[0].type_name) ==
          ".msg2.Holder.E");
}

// The accepting directions the rule must not break: a proto2 field may use a
// proto2 enum across an import (the plan-4 pair's own shape, C5), a proto2
// field may use a proto3 enum (S1), and a proto3 field may use a proto3 enum
// from its own file (S3). A mutation keying on the requesting file alone fails
// the first two; one keying on the provider regardless of the request fails
// the second.
void test_proto2_fields_may_use_either_enum_syntax()
{
    {
        easypb_schema::SchemaSet set;
        make_enum_provider(set, "base2.proto", "shared", "proto2");
        easypb_schema::SchemaFile& root = make_enum_consumer(
            set, "root.proto", "app", "proto2", "base2.proto",
            FieldDescriptorProto::TYPE_ENUM, ".shared.State");

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        CHECK(root.file.message_type[0].field[0].type ==
              FieldDescriptorProto::TYPE_ENUM);
        CHECK(text(root.file.message_type[0].field[0].type_name) ==
              ".shared.State");
    }
    {
        easypb_schema::SchemaSet set;
        make_enum_provider(set, "base3.proto", "base3", "proto3");
        easypb_schema::SchemaFile& root = make_enum_consumer(
            set, "root.proto", "app", "proto2", "base3.proto",
            FieldDescriptorProto::TYPE_ENUM, ".base3.State");

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        CHECK(text(root.file.message_type[0].field[0].type_name) ==
              ".base3.State");
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& root =
            make_simple_file(set, "root.proto", "app");
        root.from_descriptor_set = true;
        set_syntax(root, "proto3");
        add_enum(root, "State", "STATE_UNKNOWN");
        DescriptorProto u;
        u.name = root.strings.save("U");
        u.has_name = true;
        add_typed_field(root, u, "s", 1,
                        FieldDescriptorProto::LABEL_OPTIONAL,
                        FieldDescriptorProto::TYPE_ENUM, ".app.State");
        root.file.message_type.push_back(u);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        CHECK(text(root.file.message_type[0].field[0].type_name) ==
              ".app.State");
    }
}

// The A25 precedence, asserted rather than assumed: classification runs before
// any resolution, so a set that also contains the proto3-uses-proto2-enum
// shape answers DIAGNOSTIC_UNSUPPORTED_SYNTAX and never the enum-openness
// verdict (Step 6.2's "the last two rows are not the cross-syntax rule"). The
// editions file is the offending consumer in the first block and a later-
// named sibling in the second; both carry A25's own sentence, which is what
// shows the cross-syntax rule never got to answer.
void test_unsupported_syntax_beats_the_cross_syntax_rule()
{
    {
        easypb_schema::SchemaSet set;
        make_enum_provider(set, "base2.proto", "base2", "proto2");
        make_enum_consumer(set, "a.proto", "app", "editions", "base2.proto",
                           FieldDescriptorProto::TYPE_ENUM, ".base2.State");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
        CHECK(error.file == "a.proto");
        CHECK(error.message.find("is not supported") != std::string::npos);
    }
    {
        easypb_schema::SchemaSet set;
        make_enum_provider(set, "base2.proto", "base2", "proto2");
        make_enum_consumer(set, "a.proto", "app", "proto3", "base2.proto",
                           FieldDescriptorProto::TYPE_ENUM, ".base2.State");
        easypb_schema::SchemaFile& editions =
            make_simple_file(set, "z.proto", 0);
        editions.from_descriptor_set = true;
        set_syntax(editions, "editions");

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                          linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
        CHECK(error.file == "z.proto");
        CHECK(linked.files == 0);
        CHECK(!linked.local_only);
    }
}

// The deferred pass resolves nothing, so the rule stays silent there: a proto3
// file whose field names a proto2 enum passes validation with its metadata
// untouched. The parser's own allow_unresolved pass resolves inside its own
// file only, whose syntax is the field's file's syntax by definition, so the
// rule cannot start rejecting files that pass accepted before their imports
// were linked.
void test_cross_syntax_rule_is_silent_in_the_deferred_pass()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& file =
        make_simple_file(set, "root.proto", "app");
    file.from_descriptor_set = true;
    set_syntax(file, "proto3");
    DescriptorProto u;
    u.name = file.strings.save("U");
    u.has_name = true;
    add_typed_field(file, u, "s", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                    FieldDescriptorProto::TYPE_ENUM, ".base2.State");
    file.file.message_type.push_back(u);

    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::validate_deferred_file(file, error));
    const FieldDescriptorProto& field = file.file.message_type[0].field[0];
    CHECK(field.type == FieldDescriptorProto::TYPE_ENUM);
    CHECK(text(field.type_name) == ".base2.State");
}

// --- Step 7: descriptor compatibility mode (Task 3, R15-R20) ---------------
// LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY is the thin `.pbs` workflow: exactly
// one descriptor-origin file, missing targets tolerated as null-target edges,
// and a finish pass that permits an unresolved reference only when the
// descriptor classifies it itself (an explicit message/enum kind and an
// absolute name). The rows below are the pack's Step 7.3 table; the Step 7.4
// corpus checkpoint is test_corpus_compatibility_mode, which receives the
// tests source directory from tests/CMakeLists.txt because the tracked `.pbs`
// fixtures live in several data directories.

// The compat twin of link_verdict: `complete` is this mode's own verdict
// (A4), so the caller reads it from `linked` and asserts it per fixture. The
// publish-on-failure rule (A1) is asserted here for every row this helper
// drives.
bool link_compat(easypb_schema::SchemaSet& set,
                 easypb_schema::LinkedSchema& linked,
                 easypb_schema::Diagnostic& error)
{
    linked = easypb_schema::LinkedSchema();
    const bool linked_ok = easypb_schema::link_schema(
        set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY, linked,
        error);
    if (linked_ok) {
        CHECK(linked.files == &set);
    } else {
        CHECK(linked.files == 0);
        CHECK(!linked.local_only);
    }
    return linked_ok;
}

// One descriptor-origin file with an optional package and optional single
// dependency: the thin `.pbs` shape every Step-7 row starts from. A null
// dependency leaves the file without an import edge; a null package leaves it
// at the global scope.
easypb_schema::SchemaFile& make_thin_file(easypb_schema::SchemaSet& set,
                                          const char* logical,
                                          const char* package,
                                          const char* dependency)
{
    std::vector<std::string> deps;
    if (dependency != 0) deps.push_back(dependency);
    const std::vector<std::int32_t> no_indexes;
    easypb_schema::SchemaFile& file =
        make_file(set, logical, deps, no_indexes, no_indexes);
    file.from_descriptor_set = true;
    if (package != 0) set_package(file, package);
    return file;
}

// Reads one binary fixture whole. Descriptor sets are small tracked files,
// and the test needs the bytes alive for as long as the decoded views are
// (C++17 keeps str_view pointing into this buffer), so the caller keeps the
// string on its own stack frame.
bool read_binary_file(const std::string& path, std::string& contents)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    contents.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

// 7.3 row 1 and 2: a classified external message is accepted even though its
// dependency is absent, `local_only == false` comes from the null-target edge
// alone (A4), and the same input under LINK_COMPLETE is a missing import.
void test_compat_accepts_a_classified_external_message()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_thin_file(set, "thin.pbs", "app", "missing.proto");
        DescriptorProto u;
        u.name = file.strings.save("U");
        u.has_name = true;
        add_typed_field(file, u, "t", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                        FieldDescriptorProto::TYPE_MESSAGE, ".ext.Type");
        file.file.message_type.push_back(u);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(link_compat(set, linked, error));
        CHECK(!linked.local_only);
        CHECK(file.warnings.size() == 1);
        CHECK(file.warnings[0].warning);
        CHECK(file.warnings[0].code == easypb_schema::DIAGNOSTIC_GENERIC);
        CHECK(file.warnings[0].message.find(".ext.Type") != std::string::npos);
        CHECK(file.warnings[0].message.find("were not validated") !=
              std::string::npos);
        // A5's stable location: a thin descriptor carries no position, so the
        // warning must not pretend to have one.
        CHECK(file.warnings[0].location.line == 0);
        CHECK(file.warnings[0].location.column == 0);
        // The permitted reference keeps its declared metadata verbatim: no
        // fabricated kind, no rewritten name.
        const FieldDescriptorProto& field = file.file.message_type[0].field[0];
        CHECK(field.has_type &&
              field.type == FieldDescriptorProto::TYPE_MESSAGE);
        CHECK(text(field.type_name) == ".ext.Type");
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_thin_file(set, "thin.pbs", "app", "missing.proto");
        DescriptorProto u;
        u.name = file.strings.save("U");
        u.has_name = true;
        add_typed_field(file, u, "t", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                        FieldDescriptorProto::TYPE_MESSAGE, ".ext.Type");
        file.file.message_type.push_back(u);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
    }
}

// Fix round r12, D-25: the compatibility tolerance is for a MISSING target
// only. A reference the index HOLDS but the requesting file cannot see (an
// invisible declared type) is refused with DIAGNOSTIC_INVISIBLE_TYPE - the
// same verdict complete mode gives the shape and protoc's own
// undeclared-dependency error class - so the A5 warning can never state that
// a type "is not declared in the linked set" when the set declares it. The
// shipped single-file caller cannot build this shape (its index holds one
// file, Visible = that file); the test drives the wider public signature the
// review probed, which is why this is a resolve_file_compat row and not a
// link_schema one.
void test_compat_refuses_an_invisible_reference()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& a = set.add_file("inv.a.pbs");
    a.from_descriptor_set = true;
    a.file.syntax = a.strings.save("proto2");
    a.file.has_syntax = true;
    a.file.package = a.strings.save("pkg");
    a.file.has_package = true;
    {
        DescriptorProto use;
        use.name = a.strings.save("Use");
        use.has_name = true;
        add_typed_field(a, use, "f", 1,
                        FieldDescriptorProto::LABEL_OPTIONAL,
                        FieldDescriptorProto::TYPE_MESSAGE, ".pkg.Target");
        a.file.message_type.push_back(use);
    }
    easypb_schema::SchemaFile& b = set.add_file("inv.b.pbs");
    b.from_descriptor_set = true;
    b.file.syntax = b.strings.save("proto2");
    b.file.has_syntax = true;
    b.file.package = b.strings.save("pkg");
    b.file.has_package = true;
    {
        DescriptorProto target;
        target.name = b.strings.save("Target");
        target.has_name = true;
        b.file.message_type.push_back(target);
    }

    // Complete mode refuses the reference as INVISIBLE.
    easypb_schema::Diagnostic error;
    CHECK(!link_verdict(set, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);

    // The compatibility finish pass over a WIDER index (both files indexed,
    // Visible = {a}) must refuse the same reference with the same code.
    easypb_schema::SymbolIndex symbols;
    CHECK(easypb_schema::index_file(a, symbols, error));
    CHECK(easypb_schema::index_file(b, symbols, error));
    std::set<const easypb_schema::SchemaFile*> visible;
    visible.insert(&a);
    std::set<std::string> warned_types;
    bool missing_external = false;
    CHECK(!easypb_schema::resolve_file_compat(a, symbols, visible,
                                              warned_types, missing_external,
                                              error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE);
    // The diagnostic names the invisible declarer (its logical name, through
    // defining_file_name) and states the visibility fact, not a missing
    // declaration.
    CHECK(error.message.find("not visible") != std::string::npos);
    CHECK(error.message.find("inv.b") != std::string::npos);
    CHECK(!missing_external);
    CHECK(warned_types.empty());
    CHECK(a.warnings.empty());
    // Nothing was rewritten before the refusal.
    CHECK(text(a.file.message_type[0].field[0].type_name) == ".pkg.Target");
}

// 7.3 row 3: the dependency descriptor is absent while every field resolves
// locally. The first edition's `complete` would have read true here; A4 ties
// the flag to the null-target edge, which is the input's own fact.
void test_compat_missing_dependency_without_external_references()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& file =
        make_thin_file(set, "thin.pbs", "app", "missing.proto");
    add_enum(file, "State", "STATE_UNKNOWN");
    DescriptorProto u;
    u.name = file.strings.save("U");
    u.has_name = true;
    add_typed_field(file, u, "s", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                    FieldDescriptorProto::TYPE_ENUM, ".app.State");
    file.file.message_type.push_back(u);

    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(link_compat(set, linked, error));
    CHECK(!linked.local_only);
    CHECK(file.warnings.empty());
    const FieldDescriptorProto& field = file.file.message_type[0].field[0];
    CHECK(field.type == FieldDescriptorProto::TYPE_ENUM);
    CHECK(text(field.type_name) == ".app.State");
}

// 7.3 row 4: two fields naming one external type produce exactly ONE warning
// (A5), at the stable unknown location - `warn_once` alone would emit one per
// field, because it appends each field's descriptor path when the position is
// unknown.
void test_compat_warns_once_per_external_type_name()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& file =
        make_thin_file(set, "thin.pbs", "app", "missing.proto");
    DescriptorProto u;
    u.name = file.strings.save("U");
    u.has_name = true;
    add_typed_field(file, u, "a", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                    FieldDescriptorProto::TYPE_MESSAGE, ".ext.Type");
    add_typed_field(file, u, "b", 2, FieldDescriptorProto::LABEL_OPTIONAL,
                    FieldDescriptorProto::TYPE_MESSAGE, ".ext.Type");
    file.file.message_type.push_back(u);

    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(link_compat(set, linked, error));
    CHECK(!linked.local_only);
    CHECK(file.warnings.size() == 1);
    CHECK(file.warnings[0].location.line == 0);
    CHECK(file.warnings[0].message.find(".ext.Type") != std::string::npos);
}

// 7.3 rows 5 and 6: the tolerance never covers an unclassified source-style
// name. A relative external name is refused, and so is an absolute name whose
// descriptor carries no explicit kind - the mode exists to preserve thin
// descriptors, not to accept anything unresolved.
void test_compat_rejects_unclassified_external_references()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_thin_file(set, "thin.pbs", "app", "missing.proto");
        DescriptorProto u;
        u.name = file.strings.save("U");
        u.has_name = true;
        add_typed_field(file, u, "t", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                        FieldDescriptorProto::TYPE_MESSAGE, "ext.Type");
        file.file.message_type.push_back(u);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        // The category is still the lookup outcome's own: the reference is
        // missing, and the policy merely refuses to tolerate it.
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        CHECK(error.message.find("compatibility mode") != std::string::npos);
        CHECK(error.message.find("absolute type name") != std::string::npos);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_thin_file(set, "thin.pbs", "app", "missing.proto");
        DescriptorProto u;
        u.name = file.strings.save("U");
        u.has_name = true;
        FieldDescriptorProto field;
        field.name = file.strings.save("t");
        field.has_name = true;
        field.number = 1;
        field.has_number = true;
        field.label = FieldDescriptorProto::LABEL_OPTIONAL;
        field.has_label = true;
        field.type_name = file.strings.save(".ext.Type");
        field.has_type_name = true;
        u.field.push_back(field);
        file.file.message_type.push_back(u);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        // The descriptor names a type but declares no kind, so the declared
        // shape is refused BEFORE the compatibility mode's own rule is asked:
        // the mode tolerates an unresolved reference only when the descriptor
        // supplies an explicit message or enum kind, and a field with no kind
        // at all cannot supply one (the plan-5 follow-up's S1, W-10).
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_FIELD_TYPE);
    }
}

// 7.3 row 7: a name that exists locally is resolved normally - `complete` is
// unaffected by it, and no warning is recorded, which is the difference
// between an external reference and a local one under this mode.
void test_compat_resolves_local_references_normally()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& file = make_thin_file(set, "thin.pbs", "app", 0);
    add_enum(file, "State", "STATE_UNKNOWN");
    DescriptorProto u;
    u.name = file.strings.save("U");
    u.has_name = true;
    add_typed_field(file, u, "s", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                    FieldDescriptorProto::TYPE_ENUM, ".app.State");
    file.file.message_type.push_back(u);

    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(link_compat(set, linked, error));
    CHECK(linked.local_only);
    CHECK(file.warnings.empty());
    const FieldDescriptorProto& field = file.file.message_type[0].field[0];
    CHECK(field.type == FieldDescriptorProto::TYPE_ENUM);
    CHECK(text(field.type_name) == ".app.State");
    // Visible(F) degenerates to {F} in this mode: the single file is the
    // whole visible set.
    CHECK(linked.visible_files.size() == 1);
    CHECK(linked.visible_files.find(&file) != linked.visible_files.end());
}

// 7.3 row 8: the declared-kind check is the SAME one complete mode runs (A7,
// written once in resolve_message), so a descriptor that declares the wrong
// kind for a locally available definition fails with
// DIAGNOSTIC_KIND_MISMATCH in both modes.
void test_compat_kind_mismatch_is_the_complete_mode_code()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_thin_file(set, "thin.pbs", "app", 0);
        add_message(file, "M");
        DescriptorProto u;
        u.name = file.strings.save("U");
        u.has_name = true;
        add_typed_field(file, u, "m", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                        FieldDescriptorProto::TYPE_ENUM, ".app.M");
        file.file.message_type.push_back(u);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_KIND_MISMATCH);
        CHECK(error.message.find(".app.M") != std::string::npos);
    }
    {
        // The mirror, plus the complete-mode twin of the same fixture: one
        // code, both modes.
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_thin_file(set, "thin.pbs", "app", 0);
        add_enum(file, "E", "A");
        DescriptorProto u;
        u.name = file.strings.save("U");
        u.has_name = true;
        add_typed_field(file, u, "e", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                        FieldDescriptorProto::TYPE_MESSAGE, ".app.E");
        file.file.message_type.push_back(u);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_KIND_MISMATCH);

        easypb_schema::SchemaSet complete_set;
        easypb_schema::SchemaFile& complete_file =
            make_thin_file(complete_set, "thin.pbs", "app", 0);
        add_enum(complete_file, "E", "A");
        DescriptorProto complete_u;
        complete_u.name = complete_file.strings.save("U");
        complete_u.has_name = true;
        add_typed_field(complete_file, complete_u, "e", 1,
                        FieldDescriptorProto::LABEL_OPTIONAL,
                        FieldDescriptorProto::TYPE_MESSAGE, ".app.E");
        complete_file.file.message_type.push_back(complete_u);
        easypb_schema::Diagnostic complete_error;
        CHECK(!link_verdict(complete_set, complete_error));
        CHECK(complete_error.code == easypb_schema::DIAGNOSTIC_KIND_MISMATCH);
    }
}

// 7.3 row 9: A26's entry-shape sweep runs before the finish pass in this mode
// too - the orphan entry is rejected without any field referencing it, and a
// referencing field does not change the verdict, because the shape is proved
// from the available descriptor first.
void test_compat_runs_the_entry_shape_sweep_before_the_finish_pass()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_thin_file(set, "malformed_map.pbs", "easypb.test",
                           "missing.proto");
        DescriptorProto broken;
        broken.name = file.strings.save("BrokenMap");
        broken.has_name = true;
        add_map_entry_key_only(file, broken, "ItemsEntry");
        file.file.message_type.push_back(broken);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MALFORMED_MAP_ENTRY);
        CHECK(error.file == "malformed_map.pbs");
        CHECK(error.message.find("BrokenMap.ItemsEntry") != std::string::npos);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_thin_file(set, "malformed_map.pbs", "easypb.test",
                           "missing.proto");
        DescriptorProto broken;
        broken.name = file.strings.save("BrokenMap");
        broken.has_name = true;
        add_map_entry_key_only(file, broken, "ItemsEntry");
        add_map_field(file, broken, "items", 1,
                      ".easypb.test.BrokenMap.ItemsEntry");
        file.file.message_type.push_back(broken);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MALFORMED_MAP_ENTRY);
        CHECK(error.message.find("BrokenMap.ItemsEntry") != std::string::npos);
    }
}

// D-23 in compatibility mode: the thin single-descriptor workflow runs the
// same structure checks, because they live in the shared resolve_message walk -
// an externally produced thin descriptor is exactly where a malformed field
// lands, and the structure has to answer before the compat classifier could
// permit the reference as external information. The codes and sentences are
// the complete mode's.
void test_compat_validates_field_structure()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_thin_file(set, "thin.pbs", "pkg", 0);
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_field(file, holder, "f", 19000, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        file.file.message_type.push_back(holder);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_FIELD_NUMBER);
        CHECK(error.message.find("19000") != std::string::npos);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_thin_file(set, "thin.pbs", "pkg", "missing.proto");
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_field(file, holder, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        holder.field[0].type = FieldDescriptorProto::TYPE_MESSAGE;
        file.file.message_type.push_back(holder);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_TYPE_NAME);
    }
}

// 7.3 rows 10-12: the 7.1 preconditions. A source-origin file is refused even
// as the only file, the set must hold EXACTLY one file (a set of two
// descriptor-origin files fails the same check as a zero-file set - A26),
// and an editions file is refused by A25 before this branch runs at all.
void test_compat_preconditions_are_exact()
{
    {
        easypb_schema::SchemaSet set;
        make_simple_file(set, "a.proto", "app");   // source-origin
        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
        CHECK(error.file == "a.proto");
        CHECK(error.message.find("from_descriptor_set") != std::string::npos);
    }
    {
        // The source-origin refusal runs before the count check, so a mixed
        // set still names the source-origin file.
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& descriptor = make_simple_file(set, "b.proto", "app");
        descriptor.from_descriptor_set = true;
        make_simple_file(set, "a.proto", "app");
        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.file == "a.proto");
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& a = make_simple_file(set, "a.proto", "app");
        a.from_descriptor_set = true;
        easypb_schema::SchemaFile& b = make_simple_file(set, "b.proto", "app");
        b.from_descriptor_set = true;
        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.file.empty());
        CHECK(error.message.find("exactly one") != std::string::npos);
        CHECK(error.message.find("2 files") != std::string::npos);
    }
    {
        // parser/data/empty.pbs decodes to exactly this: 0 bytes, 0 files.
        easypb_schema::SchemaSet set;
        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.message.find("no file at all") != std::string::npos);
    }
}

// 7.3 row 13: A25's refusal is an input-capability statement, so it answers
// before the mode's own preconditions - in compatibility mode exactly as in
// complete mode, never downgraded to proto2.
void test_compat_refuses_editions_like_complete_mode()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_thin_file(set, "thin.pbs", 0, 0);
        set_syntax(file, "editions");
        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
        CHECK(error.file == "thin.pbs");
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_thin_file(set, "thin.pbs", 0, 0);
        set_syntax(file, "editions");
        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
    }
}

// 7.3 row 14: allow_missing relaxes missing targets, not malformed indexes -
// a public_dependency index outside the dependency list, and one that repeats
// an index, are refused in this mode exactly as in complete mode (A18).
void test_compat_binder_still_rejects_malformed_indexes()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_file(
            set, "thin.pbs", std::vector<std::string>(1, "a.proto"),
            repeated_indexes(1, 5), std::vector<std::int32_t>());
        file.from_descriptor_set = true;
        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
        CHECK(error.message.find("public dependency index") !=
              std::string::npos);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_file(
            set, "thin.pbs", std::vector<std::string>(1, "a.proto"),
            repeated_indexes(2, 0), std::vector<std::int32_t>());
        file.from_descriptor_set = true;
        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!link_compat(set, linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
        CHECK(error.message.find("duplicate public") != std::string::npos);
    }
}

// 7.3 row 15: a weak import of an absent file is tolerated exactly like a
// normal missing target (A18): a null-target edge and `local_only == false`,
// never silence - while the same fixture under LINK_COMPLETE stays the hard
// error it has always been.
void test_compat_tolerates_a_missing_weak_target()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_file(
            set, "thin.pbs", std::vector<std::string>(1, "gone.proto"),
            std::vector<std::int32_t>(), repeated_indexes(1, 0));
        file.from_descriptor_set = true;
        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(link_compat(set, linked, error));
        CHECK(!linked.local_only);
        CHECK(file.warnings.empty());
        CHECK(file.edges.size() == 1);
        CHECK(file.edges[0].target == 0);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_file(
            set, "thin.pbs", std::vector<std::string>(1, "gone.proto"),
            std::vector<std::int32_t>(), repeated_indexes(1, 0));
        file.from_descriptor_set = true;
        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
    }
}

// 7.3 row 16: an explicit imported-enum default is preserved verbatim - no
// first enumerator, no integer zero - and the only diagnostic is the one
// external-type warning (A5). The default is NOT validated against the
// external enum, because the external definition cannot answer; 7.2's own
// warning text states that defaults were not validated.
void test_compat_preserves_an_external_enum_default()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& file =
        make_thin_file(set, "thin.pbs", "app", "missing.proto");
    DescriptorProto u;
    u.name = file.strings.save("U");
    u.has_name = true;
    add_typed_field(file, u, "state", 1,
                    FieldDescriptorProto::LABEL_OPTIONAL,
                    FieldDescriptorProto::TYPE_ENUM, ".ext.State");
    u.field[0].default_value = file.strings.save("READY");
    u.field[0].has_default_value = true;
    file.file.message_type.push_back(u);

    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(link_compat(set, linked, error));
    CHECK(!linked.local_only);
    CHECK(file.warnings.size() == 1);
    CHECK(file.warnings[0].code == easypb_schema::DIAGNOSTIC_GENERIC);
    const FieldDescriptorProto& field = file.file.message_type[0].field[0];
    CHECK(field.has_default_value && text(field.default_value) == "READY");
    CHECK(field.type == FieldDescriptorProto::TYPE_ENUM);
    CHECK(text(field.type_name) == ".ext.State");
    // Nothing fabricated: the file declares no enumerator of its own, and no
    // zero-valued member appeared where the external enum's first value would
    // be.
    CHECK(file.file.enum_type.empty());
}

// 7.3 row 17: a set that linked under compatibility mode does not teach
// LINK_COMPLETE anything - the strict mode rebuilds the graph and rejects the
// same input for the same reason it always did. The compat path is never
// consulted from LINK_COMPLETE.
void test_compat_then_complete_never_consults_the_escape_hatch()
{
    easypb_schema::SchemaSet set;
    easypb_schema::SchemaFile& file =
        make_thin_file(set, "thin.pbs", "app", "missing.proto");
    DescriptorProto u;
    u.name = file.strings.save("U");
    u.has_name = true;
    add_typed_field(file, u, "t", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                    FieldDescriptorProto::TYPE_MESSAGE, ".ext.Type");
    file.file.message_type.push_back(u);

    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(link_compat(set, linked, error));
    CHECK(!linked.local_only);

    easypb_schema::Diagnostic complete_error;
    CHECK(!link_verdict(set, complete_error));
    CHECK(complete_error.code == easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
}

// --- Step 7.4: the tracked `.pbs` corpus through compatibility mode (A23) --
// Appendix D's inventory, measured before Step 1: 31 tracked fixtures = 26
// clean single files + malformed_map (A26's shape rule) + custom_types
// (dependency absent, no external reference) + external (dependency absent,
// external references) + empty (no file) + integration-multi (two files).
enum CorpusVerdict {
    CORPUS_CLEAN,             // accept, local_only == true, no warnings
    CORPUS_INCOMPLETE,        // accept, local_only == false
    CORPUS_PRECONDITION,      // reject at 7.1 (no file / two files)
    CORPUS_MALFORMED_ENTRY    // reject at A26's entry-shape check
};

struct CorpusFixture
{
    const char* relative;     // under the tests source directory
    CorpusVerdict verdict;
    std::size_t warnings;     // expected warnings when accepted
};

const CorpusFixture kCorpusFixtures[] = {
    {"codegen/custom_types/data/cpp_group_without_type.pbs", CORPUS_CLEAN, 0},
    {"codegen/custom_types/data/custom_types.pbs", CORPUS_INCOMPLETE, 0},
    {"codegen/custom_types/data/empty_cpp_type.pbs", CORPUS_CLEAN, 0},
    {"codegen/custom_types/data/foreign_51000.pbs", CORPUS_CLEAN, 0},
    {"codegen/custom_types/data/template_cases.pbs", CORPUS_CLEAN, 0},
    {"codegen/enums/data/enum_codegen.pbs", CORPUS_CLEAN, 0},
    {"codegen/enums/data/open_enum.pbs", CORPUS_CLEAN, 0},
    {"codegen/maps/data/enum_map.pbs", CORPUS_CLEAN, 0},
    {"codegen/maps/data/malformed_map.pbs", CORPUS_MALFORMED_ENTRY, 0},
    {"codegen/maps/data/message_map.pbs", CORPUS_CLEAN, 0},
    {"codegen/maps/data/scalar_maps.pbs", CORPUS_CLEAN, 0},
    {"codegen/packages/data/alpha.pbs", CORPUS_CLEAN, 0},
    {"codegen/packages/data/beta.pbs", CORPUS_CLEAN, 0},
    {"codegen/packages/data/external.pbs", CORPUS_INCOMPLETE, 2},
    {"codegen/packages/data/import-package.pbs", CORPUS_CLEAN, 0},
    {"codegen/packages/data/invalid-std.pbs", CORPUS_CLEAN, 0},
    {"codegen/packages/data/module-package.pbs", CORPUS_CLEAN, 0},
    {"codegen/packages/data/names.pbs", CORPUS_CLEAN, 0},
    {"codegen/packages/data/shadow.pbs", CORPUS_CLEAN, 0},
    {"codegen/parser/data/empty.pbs", CORPUS_PRECONDITION, 0},
    {"codegen/parser/data/integration-multi.pbs", CORPUS_PRECONDITION, 0},
    {"codegen/parser/data/integration-proto2.pbs", CORPUS_CLEAN, 0},
    {"codegen/parser/data/integration-proto3.pbs", CORPUS_CLEAN, 0},
    {"codegen/parser/data/nested-ancestor-recursive.pbs", CORPUS_CLEAN, 0},
    {"codegen/parser/data/nested-forward.pbs", CORPUS_CLEAN, 0},
    {"codegen/parser/data/nested-lexical-cycle.pbs", CORPUS_CLEAN, 0},
    {"codegen/parser/data/nested-messages.pbs", CORPUS_CLEAN, 0},
    {"codegen/parser/data/nested-mutual.pbs", CORPUS_CLEAN, 0},
    {"codegen/parser/data/nested-recursive-map.pbs", CORPUS_CLEAN, 0},
    {"codegen/parser/data/nested-recursive-repeated.pbs", CORPUS_CLEAN, 0},
    {"codegen/parser/data/nested-recursive.pbs", CORPUS_CLEAN, 0}

};

const std::size_t kCorpusFixtureCount =
    sizeof(kCorpusFixtures) / sizeof(kCorpusFixtures[0]);

// The Step 7.4 corpus checkpoint (A23): every tracked `.pbs` fixture goes
// through compatibility mode with the verdict and warning count Appendix D
// measured, in the parser-free suite. One SchemaSet per fixture: the mode's
// warranty is per descriptor, and folding several `--descriptor-set` inputs
// into one set is exactly the frontend mistake 7.1 refuses.
void test_corpus_compatibility_mode(const std::string& fixture_dir)
{
    std::size_t classifiable = 0;
    for (std::size_t i = 0; i < kCorpusFixtureCount; ++i) {
        const CorpusFixture& fixture = kCorpusFixtures[i];
        const std::string path = fixture_dir + "/" + fixture.relative;
        std::string bytes;
        CHECK(read_binary_file(path, bytes));

        const FileDescriptorSet decoded =
            easypb::decode<FileDescriptorSet>(bytes);
        // P-33 (the standing form of the round r27 corpus sweep): the tracked
        // corpus is the regression input for the D-62 decoder half. The sweep
        // must never fire on a fixture the repository tracks - the clause is
        // for CRAFTED synthetic entries - so every map entry here must read
        // both extension counters as zero. A non-zero counter means either the
        // model started counting wire fields 5/6 for another purpose or a
        // fixture grew extension data; both need a decision, and a silent one
        // would move corpus verdicts without a green flag moving.
        for (std::size_t j = 0; j < decoded.file.size(); ++j) {
            const FileDescriptorProto& corpus_file = decoded.file[j];
            for (std::size_t k = 0; k < corpus_file.message_type.size(); ++k) {
                const DescriptorProto& message = corpus_file.message_type[k];
                const bool entry = message.options.has_map_entry &&
                                   message.options.map_entry;
                if (entry) {
                    CHECK(message.extension_range.empty());
                    CHECK(message.extension_count == 0);
                }
            }
        }
        easypb_schema::SchemaSet set;
        for (std::size_t j = 0; j < decoded.file.size(); ++j) {
            const std::string logical = text(decoded.file[j].name);
            easypb_schema::SchemaFile& file = set.add_file(logical);
            file.physical_name.clear();
            file.from_descriptor_set = true;
            // The decoded views point into `bytes`, which outlives the link
            // inside this iteration; C++11 copies the str_views into the
            // descriptor, C++17 keeps the views themselves.
            file.file = decoded.file[j];
        }
        if (!decoded.file.empty()) ++classifiable;

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        const bool accepted = easypb_schema::link_schema(
            set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY,
            linked, error);
        if (accepted) {
            CHECK(linked.files == &set);
        } else {
            CHECK(linked.files == 0);
            CHECK(!linked.local_only);
            // The checkpoint's non-count assertion (A25): NO fixture may be
            // refused as an unsupported syntax. Such a refusal would mean the
            // classifier turned a supported value into an unsupported one,
            // and every thin `.pbs` consumer would stop. The refusals that
            // remain are 7.1's own: empty.pbs has no file to classify, and
            // integration-multi.pbs holds two.
            CHECK(error.code != easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
        }

        switch (fixture.verdict) {
        case CORPUS_CLEAN:
            CHECK(accepted);
            if (accepted) {
                CHECK(linked.local_only);
                CHECK(set.files()[0]->warnings.empty());
            }
            break;
        case CORPUS_INCOMPLETE:
            CHECK(accepted);
            if (accepted) {
                CHECK(!linked.local_only);
                CHECK(set.files()[0]->warnings.size() == fixture.warnings);
            }
            break;
        case CORPUS_PRECONDITION:
            CHECK(!accepted);
            break;
        case CORPUS_MALFORMED_ENTRY:
            CHECK(!accepted);
            if (!accepted) {
                CHECK(error.code ==
                      easypb_schema::DIAGNOSTIC_MALFORMED_MAP_ENTRY);
                // The decoded file's own logical name, not the .pbs path.
                CHECK(error.file == "malformed_map.proto");
                // The entry name, not the full sentence: the generator's
                // CMake assertion (test_codegen_maps.cmake:139) keeps pinning
                // the wording, so this test and it share it instead of both
                // freezing a copy.
                CHECK(error.message.find("BrokenMap.ItemsEntry") !=
                      std::string::npos);
            }
            break;
        }

        if (accepted && std::string(fixture.relative) ==
                            "codegen/packages/data/external.pbs") {
            // Finding 2 / R19: the external enum default is preserved
            // verbatim - nothing fabricates a first enumerator or an integer
            // zero - and A5's bound produced ONE warning for the two fields
            // sharing `.other.State`; the second warning is for
            // `.other.External`. A per-field warn_once would have produced
            // three references -> three warnings (the sweep's measured
            // comparison).
            const easypb_schema::SchemaFile& file = *set.files()[0];
            const FieldDescriptorProto& state =
                file.file.message_type[0].field[0];
            CHECK(text(state.name) == "state");
            CHECK(state.has_default_value &&
                  text(state.default_value) == "READY");
            CHECK(state.type == FieldDescriptorProto::TYPE_ENUM);
            CHECK(text(state.type_name) == ".other.State");
            CHECK(file.file.enum_type.empty());
            std::size_t state_warnings = 0;
            for (std::size_t w = 0; w < file.warnings.size(); ++w) {
                if (file.warnings[w].message.find("\".other.State\"") !=
                    std::string::npos) {
                    ++state_warnings;
                }
            }
            CHECK(state_warnings == 1);
        }
    }
    // 26 proto3 + 4 proto2 files are classifiable; `empty.pbs` holds no file
    // at all, which is why this count is 30 rather than 31.
    CHECK(classifiable == 30);
}

// ---------------------------------------------------------------------------
// The 0927 gemini/muse review pass (2026-09-27) reported four shapes that do
// not reproduce on this tree. The measurements are kept as rows here, so the
// next review reads the verdict instead of re-deriving it (ledger P-18/P-19),
// and so a regression that reintroduced one of them would fail a suite rather
// than wait for an instrument to be rebuilt.
// ---------------------------------------------------------------------------

// F-M1 (refuted): a map entry nested two levels down is swept like any other.
// `validate_message_map_entries` has recursed through every `nested_type`
// unconditionally since step 05, so the marker is checked at every depth and
// the diagnostic names the full descriptor path. Measured on libprotoc 30.1
// for the same shape: the set is refused too (`map_entry should not be set
// explicitly. Use map<KeyType, ValueType> instead.`) - the same verdict in
// the reference compiler's own words.
void test_map_entry_sweep_reaches_every_depth()
{
    // Outer.Mid.Deep marked as an entry and carrying only the key member.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "deep_entry.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto outer;
        outer.name = file.strings.save("Outer");
        outer.has_name = true;
        add_nested(file, outer, "Mid");
        add_map_entry_key_only(file, outer.nested_type[0], "Deep");
        file.file.message_type.push_back(outer);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MALFORMED_MAP_ENTRY);
        CHECK(error.message ==
              "Malformed map entry .pkg.Outer.Mid.Deep: fields 1 and 2 are "
              "required");
    }
    // Four levels down is the same verdict: the walk is recursive, not
    // depth-bounded.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "deep_entry.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto outer;
        outer.name = file.strings.save("Outer");
        outer.has_name = true;
        add_nested(file, outer, "A");
        add_nested(file, outer.nested_type[0], "B");
        add_nested(file, outer.nested_type[0].nested_type[0], "C");
        add_map_entry_key_only(file,
                               outer.nested_type[0].nested_type[0]
                                   .nested_type[0],
                               "Deep");
        file.file.message_type.push_back(outer);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MALFORMED_MAP_ENTRY);
        CHECK(error.message ==
              "Malformed map entry .pkg.Outer.A.B.C.Deep: fields 1 and 2 are "
              "required");
    }
    // Control: the same path with both members links - the sweep is about the
    // key/value pair, never about the depth or the marker alone.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "deep_entry.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto outer;
        outer.name = file.strings.save("Outer");
        outer.has_name = true;
        add_nested(file, outer, "Mid");
        add_map_entry(file, outer.nested_type[0], "Deep", 0);
        file.file.message_type.push_back(outer);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
    }
}

// F-L1 (refuted as a defect, pinned as parity): two fields whose names are all
// underscores imply the SAME entry name, because that is protoc's own rule for
// every name (strip the underscores, capitalize what follows, append `Entry`)
// and an all-underscore name leaves nothing but the suffix. Measured on
// libprotoc 30.1: the pair is refused - `"Entry" is already defined in
// "gm.Holder"` - so refusing it here is the same verdict as the reference
// compiler's. The rule half lives in test_semantics.cpp's entry-name table
// (both `_` and `__` give `Entry`); this half pins the code the collision takes
// in the linker.
void test_underscore_map_fields_collide_on_the_shared_entry_name()
{
    CHECK(easypb_schema::map_entry_name("_") == "Entry");
    CHECK(easypb_schema::map_entry_name("__") == "Entry");

    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "underscore.pbs", "gm");
        file.from_descriptor_set = true;
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        // Exactly what the parser synthesizes for `map<int32, int32> _ = 1;`
        // beside `map<int32, int32> __ = 2;` in one message: two entry
        // messages, both named `Entry`.
        add_map_entry(file, holder, "Entry", 0);
        add_map_entry(file, holder, "Entry", 0);
        add_map_field(file, holder, "_", 1, ".gm.Holder.Entry");
        add_map_field(file, holder, "__", 2, ".gm.Holder.Entry");
        file.file.message_type.push_back(holder);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
        CHECK(error.message.find("Entry") != std::string::npos);
    }
    // Control: ONE all-underscore field is an ordinary legal map whose entry is
    // named `Entry` - protoc generates exactly that name (measured).
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "underscore.pbs", "gm");
        file.from_descriptor_set = true;
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_map_entry(file, holder, "Entry", 0);
        add_map_field(file, holder, "_", 1, ".gm.Holder.Entry");
        file.file.message_type.push_back(holder);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
    }
}

// F-H1 (refuted, then overridden): the muse review's HIGH finding read "a field with neither a
// declared kind nor a type_name slips past the A7/clause-12 guard, and protoc
// rejects that shape too", and proposed refusing it with MISSING_TYPE_NAME.
// Measured over a real descriptor set mutated by deleting the field's `type`
// varint with every other byte preserved: libprotoc 30.1 BUILDS that file
// (exit 0), so the proposed refusal would have created the divergence it
// claimed to close. The rows below keep both accepted shapes and the
// neighbour the same measurement settled, so a later session that wants the
// refusal has to produce a new counter-example rather than re-read the report.
//
// That later session is the plan-5 follow-up's S1 (W-10), and the owner's
// decision of 2026-10-21 takes the refusal anyway: a field that declares no
// kind has no spelling in a generated header, and the emitter used to answer
// one with "???type" at rc=0. The shape is a deliberate "stricter than
// protoc" divergence, and the measurement below is the new counter-example
// the F-H1 note asked for - protoc accepts all three shapes, this project
// refuses them.
void test_descriptor_origin_field_without_a_declared_kind_is_refused()
{
    // Neither `type` nor `type_name`: protoc reads the absent kind as its own
    // proto2 default and accepts; this project refuses the shape, because the
    // emitter has no kind to write.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "gap.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_field(file, holder, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  0);
        // The mutated bytes: the kind tag was never on the wire.
        holder.field[0].has_type = false;
        holder.field[0].type = 0;
        file.file.message_type.push_back(holder);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_FIELD_TYPE);
    }
    // A type_name with no declared kind: protoc builds the NAMED type from the
    // name (exit 0), and the link could fill the kind from the definition
    // (A7's guard is `has_type && ...`). The refusal is the same: the
    // descriptor does not say what the field is, and the rule asks the
    // descriptor, not the resolution.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "gap.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "Target");
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_field(file, holder, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  ".pkg.Target");
        holder.field[0].has_type = false;
        holder.field[0].type = 0;
        file.file.message_type.push_back(holder);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_FIELD_TYPE);
    }
    // Neighbour from the same measurement: a DECLARED scalar kind carrying a
    // type_name is refused by protoc (`Field with primitive type has
    // type_name.`) and here by the A7 comparison - so "no declared kind" is
    // refused too, with its own code.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "gap.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "Target");
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_field(file, holder, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  ".pkg.Target");
        holder.field[0].type = FieldDescriptorProto::TYPE_INT32;
        file.file.message_type.push_back(holder);

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_KIND_MISMATCH);
    }
}

// --- D-28 (fix round r13, contract clause 12) -------------------------------
//
// Two shapes a descriptor can carry, both provable from the descriptor alone
// and both accepted where protoc refuses them: an enum whose two values share
// a number with no `option allow_alias = true`, and a oneof declaration no
// field points at. The rule is not a reference question, so it must answer the
// SAME way in both modes - A26's entry-shape sweep is the neighbouring
// reference-free check and the precedent the site follows.
//
// The two helpers below are that claim, not a convenience: a row that only
// asked complete mode would leave compatibility mode free to accept the same
// available descriptor with a different verdict.
bool refused_in_both_modes(easypb_schema::SchemaSet& set,
                           easypb_schema::Diagnostic& error)
{
    const bool complete_ok = link_verdict(set, error);
    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic compat_error;
    const bool compat_ok = easypb_schema::link_schema(
        set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY, linked,
        compat_error);
    CHECK(!complete_ok);
    CHECK(!compat_ok);
    if (!compat_ok) {
        // One category, one code (E1/A3): the same available descriptor takes
        // the same code and the same sentence in both modes.
        CHECK(compat_error.code == error.code);
        CHECK(compat_error.message == error.message);
    }
    return !complete_ok && !compat_ok;
}

bool accepted_in_both_modes(easypb_schema::SchemaSet& set,
                            easypb_schema::Diagnostic& error)
{
    const bool complete_ok = link_verdict(set, error);
    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic compat_error;
    const bool compat_ok = easypb_schema::link_schema(
        set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY, linked,
        compat_error);
    if (!complete_ok) std::cerr << "complete mode: " << error.message << "\n";
    if (!compat_ok) std::cerr << "compat mode: " << compat_error.message << "\n";
    CHECK(complete_ok);
    CHECK(compat_ok);
    return complete_ok && compat_ok;
}

// A descriptor-origin enum with two values. option_state is the three-state
// question the rule turns on: -1 no option at all, 0 `allow_alias = false`
// written out, 1 `allow_alias = true`. The option travels in the model
// (`EnumOptions`) because without it an intentional alias - the source fixture
// `tests/codegen/enums/data/enum_codegen.proto`, whose `STARTED = 1` and
// `ACTIVE = 1` sit under the option - would be refused like the defect.
EnumDescriptorProto make_enum_with_numbers(easypb_schema::SchemaFile& file,
                                           const char* enum_name,
                                           std::int32_t first,
                                           std::int32_t second,
                                           int option_state)
{
    EnumDescriptorProto enumeration;
    enumeration.name = file.strings.save(enum_name);
    enumeration.has_name = true;
    const char* names[2] = {"A", "B"};
    const std::int32_t numbers[2] = {first, second};
    for (int i = 0; i < 2; ++i) {
        EnumValueDescriptorProto value;
        value.name = file.strings.save(names[i]);
        value.has_name = true;
        value.number = numbers[i];
        value.has_number = true;
        enumeration.value.push_back(value);
    }
    if (option_state >= 0) {
        enumeration.options.allow_alias = option_state == 1;
        enumeration.options.has_allow_alias = true;
        enumeration.has_options = true;
    }
    return enumeration;
}

void add_oneof_decl(easypb_schema::SchemaFile& file, DescriptorProto& message,
                    const char* name)
{
    OneofDescriptorProto oneof;
    oneof.name = file.strings.save(name);
    oneof.has_name = true;
    message.oneof_decl.push_back(oneof);
}

void claim_oneof(DescriptorProto& message, std::size_t field_index,
                 int oneof_index)
{
    message.field[field_index].oneof_index = oneof_index;
    message.field[field_index].has_oneof_index = true;
}

// The enum half. protoc refuses the first shape when it builds a descriptor
// ("\"pkg.B\" uses the same enum value as \"pkg.A\". If this is intended, set
// 'option allow_alias = true;'") and the parser refuses it as source text, so
// what lands here is the descriptor-origin half of that frontend parity rule.
void test_descriptor_origin_duplicate_enum_number_is_refused()
{
    // (a) A = 1 and B = 1 with no option: the reported shape. The sentence
    // names both values and the number, and the descriptor path is appended
    // because a thin descriptor carries no source location.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "alias.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.enum_type.push_back(make_enum_with_numbers(file, "E", 1, 1, -1));

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_ENUM_NUMBER);
        CHECK(error.file == "alias.pbs");
        CHECK(error.message.find(
                  "enum value \"B\" of \".pkg.E\" repeats number 1, which "
                  "\"A\" already uses; set option allow_alias = true to allow "
                  "aliases") == 0);
        CHECK(error.message.find(" at [") != std::string::npos);
    }
    // (b) The same shape in a nested enum: the walk recurses, so a message
    // nothing references cannot carry it past the gate either.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "nested.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto outer;
        outer.name = file.strings.save("Outer");
        outer.has_name = true;
        DescriptorProto inner;
        inner.name = file.strings.save("Inner");
        inner.has_name = true;
        inner.enum_type.push_back(
            make_enum_with_numbers(file, "E", 7, 7, -1));
        outer.nested_type.push_back(inner);
        file.file.message_type.push_back(outer);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_ENUM_NUMBER);
        CHECK(error.message.find("of \".pkg.Outer.Inner.E\"") !=
              std::string::npos);
    }
    // (c) An intentional alias: the option present and true keeps the same two
    // numbers legal in both modes. This row is the parity control for the
    // source fixture with the same shape, so the fix cannot close D-28 by
    // refusing the aliased input too.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "aliased.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.enum_type.push_back(make_enum_with_numbers(file, "E", 1, 1, 1));

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
        CHECK(file.file.enum_type[0].options.has_allow_alias);
    }
    // (d) The option present and false is not a licence: still refused.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "noalias.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.enum_type.push_back(make_enum_with_numbers(file, "E", 1, 1, 0));

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_ENUM_NUMBER);
    }
    // (e) Distinct numbers stay legal with and without the option, so the rule
    // refuses a repeated number rather than the option's presence.
    for (int option_state = -1; option_state <= 0; ++option_state) {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "distinct.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.enum_type.push_back(
            make_enum_with_numbers(file, "E", 1, 2, option_state));

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    // (f) A one-value enum is not this rule's business: it repeats no number.
    // The empty-enum half of this row used to be accepted here as well - it
    // moved to `test_descriptor_origin_empty_enum_is_refused` in fix round r16,
    // because an enum with no values is a shape protoc refuses and `D-32`
    // closed that acceptance (the row's own note, "an enum with no values at
    // all are not this rule's business", was true of THIS rule and false of
    // the file's validity).
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "single.pbs", "pkg");
        file.from_descriptor_set = true;
        EnumDescriptorProto single;
        single.name = file.strings.save("S");
        single.has_name = true;
        EnumValueDescriptorProto value;
        value.name = file.strings.save("ONLY");
        value.has_name = true;
        value.number = 4;
        value.has_number = true;
        single.value.push_back(value);
        file.file.enum_type.push_back(single);

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
}

// The enum's OTHER own-shape question (fix round r16, ledger D-32, contract
// clause 12): an enum must DECLARE a value. protoc refuses an enum with no
// value when it builds a descriptor ("Enums must contain at least one
// value.") and our parser refuses the source twin as text, so the shape is
// descriptor input only - a hand-built or mutated `.pbs`, which is what this
// mode exists for. The check lives in the same reference-free sweep the
// duplicate-number rule uses, and it is asked BEFORE that rule's `allow_alias`
// early return, which is what row (c) below pins.
void test_descriptor_origin_empty_enum_is_refused()
{
    // (a) The reported shape: a top-level enum with no value. The sentence
    // states the rule and names the enum; the descriptor path is appended
    // because a thin descriptor carries no source location.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "empty.pbs", "pkg");
        file.from_descriptor_set = true;
        EnumDescriptorProto empty;
        empty.name = file.strings.save("E");
        empty.has_name = true;
        file.file.enum_type.push_back(empty);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_EMPTY_ENUM);
        CHECK(error.file == "empty.pbs");
        CHECK(error.message.find(
                  "enum \".pkg.E\" contains no values; an enum must contain "
                  "at least one value") == 0);
        CHECK(error.message.find(" at [") != std::string::npos);
    }
    // (b) The rule is about the value list, not about the syntax class: the
    // same empty enum in a file that declares proto3 is refused identically.
    // (D-33 records the open-enum rule this shape's sibling would need; the
    // two are separate questions and this check does not answer that one.)
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "empty3.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("proto3");
        file.file.has_syntax = true;
        EnumDescriptorProto empty;
        empty.name = file.strings.save("E");
        empty.has_name = true;
        file.file.enum_type.push_back(empty);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_EMPTY_ENUM);
    }
    // (c) `option allow_alias = true` is not a licence for the empty list -
    // protoc refuses this shape too (measured this round as
    // `enum-empty-aliased`), so the question is asked before the option's
    // early return. This is the row that fails if the check is placed after it.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "empty-alias.pbs", "pkg");
        file.from_descriptor_set = true;
        EnumDescriptorProto empty;
        empty.name = file.strings.save("E");
        empty.has_name = true;
        empty.options.allow_alias = true;
        empty.options.has_allow_alias = true;
        empty.has_options = true;
        file.file.enum_type.push_back(empty);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_EMPTY_ENUM);
    }
    // (d) The nested walk: a message's own enum and a deeper nested type's
    // enum are both reached, so an unreferenced declaration cannot carry the
    // shape past the gate.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "nested-empty.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto outer;
        outer.name = file.strings.save("Outer");
        outer.has_name = true;
        EnumDescriptorProto own;
        own.name = file.strings.save("Own");
        own.has_name = true;
        outer.enum_type.push_back(own);
        file.file.message_type.push_back(outer);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_EMPTY_ENUM);
        CHECK(error.message.find("enum \".pkg.Outer.Own\"") !=
              std::string::npos);

        easypb_schema::SchemaSet deep_set;
        easypb_schema::SchemaFile& deep =
            make_simple_file(deep_set, "deep-empty.pbs", "pkg");
        deep.from_descriptor_set = true;
        DescriptorProto mid;
        mid.name = deep.strings.save("Outer");
        mid.has_name = true;
        DescriptorProto inner;
        inner.name = deep.strings.save("Inner");
        inner.has_name = true;
        EnumDescriptorProto deep_enum;
        deep_enum.name = deep.strings.save("E");
        deep_enum.has_name = true;
        inner.enum_type.push_back(deep_enum);
        mid.nested_type.push_back(inner);
        deep.file.message_type.push_back(mid);

        easypb_schema::Diagnostic deep_error;
        CHECK(refused_in_both_modes(deep_set, deep_error));
        CHECK(deep_error.code == easypb_schema::DIAGNOSTIC_EMPTY_ENUM);
        CHECK(deep_error.message.find("enum \".pkg.Outer.Inner.E\"") !=
              std::string::npos);
    }
    // (e) Accept controls, so the new rule cannot pass by refusing enums in
    // general: a one-value enum, a nested enum with a value, and - the D-28
    // direction this round must not disturb - the aliased two-value enum.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "values.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.enum_type.push_back(make_enum_with_numbers(file, "E", 1, 2, -1));
        DescriptorProto outer;
        outer.name = file.strings.save("Outer");
        outer.has_name = true;
        EnumDescriptorProto nested;
        nested.name = file.strings.save("N");
        nested.has_name = true;
        EnumValueDescriptorProto value;
        value.name = file.strings.save("ONLY");
        value.has_name = true;
        value.number = 4;
        value.has_number = true;
        nested.value.push_back(value);
        outer.enum_type.push_back(nested);
        file.file.message_type.push_back(outer);

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "aliased.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.enum_type.push_back(make_enum_with_numbers(file, "E", 1, 1, 1));

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    // (f) The two rules are one sweep: a file carrying BOTH a repeated number
    // and an empty enum still reports the enum a reader meets first, in
    // declaration order - the duplicate-number row keeps its own sentence.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "both.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.enum_type.push_back(make_enum_with_numbers(file, "D", 1, 1, -1));
        EnumDescriptorProto empty;
        empty.name = file.strings.save("E");
        empty.has_name = true;
        file.file.enum_type.push_back(empty);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_ENUM_NUMBER);

        easypb_schema::SchemaSet second;
        easypb_schema::SchemaFile& other =
            make_simple_file(second, "both2.pbs", "pkg");
        other.from_descriptor_set = true;
        EnumDescriptorProto first_empty;
        first_empty.name = other.strings.save("E");
        first_empty.has_name = true;
        other.file.enum_type.push_back(first_empty);
        other.file.enum_type.push_back(
            make_enum_with_numbers(other, "D", 1, 1, -1));

        easypb_schema::Diagnostic second_error;
        CHECK(refused_in_both_modes(second, second_error));
        CHECK(second_error.code == easypb_schema::DIAGNOSTIC_EMPTY_ENUM);
    }
}

// Contract clause 12 (fix round r17, the 0927-mimo review's F2): a
// descriptor-origin file that declares `syntax = "proto3"` must obey the
// proto3 FIELD rules. protoc asks both questions of every field of a proto3
// message when it builds the file from the same bytes (`ValidateProto3Field`,
// `descriptor.cc:7917/7922`), and the parser refuses both as source text, so
// the two codes are the descriptor-origin half of the frontend parity rule.
// Measured before this round on both readers: `proto3-required` and
// `proto3-default` were ACCEPTED while protoc refused them
// (`build/plan4/fix-r17/probe/results-before.tsv`); the review's own third row
// - an open enum whose first value is not zero - stays out of scope on purpose
// (`D-33`, the contract sweep's row B6).
void test_descriptor_origin_proto3_field_rules_are_refused()
{
    // (a) `required` in a proto3 file: refused in both modes with the appended
    // code, and the sentence names the field.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "required3.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("proto3");
        file.file.has_syntax = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "a", 1,
                  FieldDescriptorProto::LABEL_REQUIRED, 0);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_PROTO3_REQUIRED_FIELD);
        CHECK(error.message.find("field \"a\" is required") !=
              std::string::npos);
    }
    // (b) The gate is the field's OWN file: the identical shape in a proto2 file
    // is legal, so a rule that read the wrong syntax class would refuse it.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "required2.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("proto2");
        file.file.has_syntax = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "a", 1,
                  FieldDescriptorProto::LABEL_REQUIRED, 0);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
    }
    // (c) An explicit default in a proto3 file, refused in both modes.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "default3.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("proto3");
        file.file.has_syntax = true;
        add_message(file, "M");
        DescriptorProto& message = file.file.message_type[0];
        add_field(file, message, "a", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        message.field[0].default_value = file.strings.save("5");
        message.field[0].has_default_value = true;

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_PROTO3_EXPLICIT_DEFAULT);
        CHECK(error.message.find("explicit default value") !=
              std::string::npos);
    }
    // (d) ... and the proto2 control of that shape links, with the default
    // validated as before.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "default2.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("proto2");
        file.file.has_syntax = true;
        add_message(file, "M");
        DescriptorProto& message = file.file.message_type[0];
        add_field(file, message, "a", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        message.field[0].default_value = file.strings.save("5");
        message.field[0].has_default_value = true;

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
    }
    // (e) Accept controls in a proto3 file: an optional scalar, a repeated
    // field and a field whose label is ABSENT (the decoder's zero is not
    // LABEL_REQUIRED, and protoc's label defaulting reads it as optional), so
    // the rule cannot pass by refusing proto3 fields in general.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "plain3.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("proto3");
        file.file.has_syntax = true;
        add_message(file, "M");
        DescriptorProto& message = file.file.message_type[0];
        add_field(file, message, "a", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        add_field(file, message, "b", 2,
                  FieldDescriptorProto::LABEL_REPEATED, 0);
        add_field(file, message, "c", 3, 0, 0);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
    }
    // (f) Precedence: the clause-12 field-structure checks are asked first, so
    // a required proto3 field that also carries a named kind with no type_name
    // answers the structure code, exactly as it did before this round.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "both-bad.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("proto3");
        file.file.has_syntax = true;
        add_message(file, "M");
        DescriptorProto& message = file.file.message_type[0];
        FieldDescriptorProto field;
        field.name = file.strings.save("m");
        field.has_name = true;
        field.number = 1;
        field.has_number = true;
        field.label = FieldDescriptorProto::LABEL_REQUIRED;
        field.has_label = true;
        field.type = FieldDescriptorProto::TYPE_MESSAGE;
        field.has_type = true;
        message.field.push_back(field);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_TYPE_NAME);
    }
    // (g) D-54: a proto3 GROUP is refused in both modes, while a plain message
    // field of the same nested type is not - the rule reads the field KIND, not
    // the file class alone.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "group3.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("proto3");
        file.file.has_syntax = true;
        add_message(file, "M");
        DescriptorProto& message = file.file.message_type[0];
        add_nested(file, message, "G");
        add_field(file, message, "g", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  ".pkg.M.G");
        message.field[0].type = FieldDescriptorProto::TYPE_GROUP;

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
        CHECK(error.message.find("group") != std::string::npos);
    }
    // (h) The proto2 control of that shape is refused too, with the kind's
    // own code: the generator has no group types at all, so a group is
    // refused in either syntax (the plan-5 follow-up's S1, W-10, and the
    // owner's decision of 2026-10-21 - a deliberate "stricter than protoc"
    // divergence, where protoc generates a group class for the shape).
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "group2.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("proto2");
        file.file.has_syntax = true;
        add_message(file, "M");
        DescriptorProto& message = file.file.message_type[0];
        add_nested(file, message, "G");
        add_field(file, message, "g", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  ".pkg.M.G");
        message.field[0].type = FieldDescriptorProto::TYPE_GROUP;

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_FIELD_TYPE);
        CHECK(error.message.find("group") != std::string::npos);
    }
}

// Contract clause 13 (fix round r18, the 0927-mimo review's F9 / ledger
// D-39): a descriptor-origin declaration name must be a legal protobuf symbol
// name - non-empty and made only of `[A-Za-z0-9_]` - which is protoc's own
// `ValidateSymbolName` rule (`descriptor.cc:5419-5438`, measured this round:
// every name shape protoc refuses, and only those). The source path cannot
// produce either shape (its tokenizer only yields identifier tokens), so what
// lands here is crafted `.pbs` input. protoc imposes NO first-character rule,
// so a digit-leading name stays ACCEPTED even though no C++ generator could
// emit it: that is the generator's question, not the linker's (clause 11).
void test_descriptor_origin_symbol_names_are_validated()
{
    // (a) An empty name and an illegal character are one category with one code
    // (E1/A3), and the sentence says which of the two it is.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "empty-name.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "");
        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
        CHECK(error.message.find("has no name") != std::string::npos);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "bad-name.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "foo-bar");
        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
        CHECK(error.message.find("is not a valid identifier") !=
              std::string::npos);
    }
    // (b) No first-character rule: protoc ACCEPTS a name that starts with a
    // digit, so the linker must too - the over-refusal P-19/F3 warned about.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "digit-name.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "1abc");
        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    // (c) Every modeled declaration kind is covered: enum, enum value, field,
    // oneof, service, nested message and a package component. Each row is a
    // single defect so the reported code is unambiguous.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "enum-name.pbs", "pkg");
        file.from_descriptor_set = true;
        add_enum(file, "", "A");
        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "enum-value-name.pbs", "pkg");
        file.from_descriptor_set = true;
        add_enum(file, "E", "a b");
        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "field-name.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "a.b", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "oneof-name.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        DescriptorProto& message = file.file.message_type[0];
        add_field(file, message, "a", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        add_oneof_decl(file, message, "o-1");
        claim_oneof(message, 0, 0);
        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "service-name.pbs", "pkg");
        file.from_descriptor_set = true;
        add_service(file, "");
        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "nested-name.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        add_nested(file, file.file.message_type[0], "n-1");
        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "package-name.pbs", "p-x");
        file.from_descriptor_set = true;
        add_message(file, "M");
        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
        CHECK(error.message.find("package component") != std::string::npos);
    }
    // (d) The rule is reference-free and runs before the neighbouring sweeps: a
    // file carrying BOTH a malformed message name and an empty enum reports the
    // NAME, because a name that is not a symbol name must not be indexed.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "both-defects.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "foo-bar");
        add_enum(file, "E", 0);
        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
    }
    // (e) Accept control: a fully valid descriptor with every kind present and
    // a dotted package links in both modes, so the rule cannot pass by refusing
    // the file shape in general.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "valid.pbs", "p.q");
        file.from_descriptor_set = true;
        add_message(file, "M");
        DescriptorProto& message = file.file.message_type[0];
        add_field(file, message, "a", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  0);
        add_oneof_decl(file, message, "o");
        claim_oneof(message, 0, 0);
        add_nested(file, message, "N");
        add_enum(file, "E", "ZERO");
        add_service(file, "S");
        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    // (f) The file's OWN name is a logical path, not a symbol: one no identifier
    // rule would accept is still legal, because protoc validates paths
    // separately and the linker must not invent a rule the reference lacks.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "not--an-identifier.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "a", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    // NOTE: the "name field entirely absent" variant of a message, field or
    // service cannot be reached through `link_schema` on the `.pbs` path -
    // `codegen/descriptor.pb.hpp` throws `missing_required_field` while it
    // DECODES those three structs, before the linker sees them (the round's
    // instrument records it as DECODE_THROW). That guard is a pre-existing
    // layer boundary outside plan 4's scope; the lexical rule owns the name
    // VALUE for every kind, and protoc refuses the absent variant too, so
    // verdict parity holds either way.
}

// The 0927-mimo review's F7: a relative reference with a trailing dot.
// `split_reference("N.")` returns an empty remainder, so the raw text used to
// be classified as a WHOLE-name reference to `N` - and when a message `N`
// existed, the field LINKED where protoc refuses it
// (`"N." is resolved to "p.N.", which is not defined`, measured this round).
// That is a false accept on crafted descriptor input, not merely a silent
// classification. No symbol can be indexed under a trailing dot, so the shape
// is MISSING by construction, and the absolute twin (`.p.N.`) already answered
// that way.
void test_trailing_dot_reference_is_unresolved_in_both_modes()
{
    // (a) The relative spelling. Both modes refuse it with the same code, and
    // the compatibility mode adds its policy sentence because a RELATIVE name
    // is not a classified external reference (D-25), so this row asks the two
    // modes separately - `refused_in_both_modes` demands identical sentences
    // and would fail on that documented difference rather than on a defect.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "dot.pbs", "p");
        file.from_descriptor_set = true;
        add_message(file, "N");
        easypb_schema::SchemaFile& root = file;
        add_message(root, "M");
        add_field(root, root.file.message_type[1], "m", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "N.");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        CHECK(error.message.find("N.") != std::string::npos);

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic compat_error;
        CHECK(!easypb_schema::link_schema(
            set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY, linked,
            compat_error));
        CHECK(compat_error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        CHECK(compat_error.message.find("N.") != std::string::npos);
    }
    // (b) The absolute twin: the same refusal, and it was already the code
    // this shape answered before the fix.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "dot-abs.pbs", "p");
        file.from_descriptor_set = true;
        add_message(file, "N");
        add_message(file, "M");
        add_field(file, file.file.message_type[1], "m", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, ".p.N.");

        easypb_schema::Diagnostic error;
        CHECK(!link_verdict(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
    }
    // (c) The accept control: the same reference without the trailing dot
    // still resolves, so the guard cannot have disabled whole-name resolution.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "nodot.pbs", "p");
        file.from_descriptor_set = true;
        add_message(file, "N");
        add_message(file, "M");
        add_field(file, file.file.message_type[1], "m", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, "N");

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
    }
}

// The oneof half: every declaration needs a field. The index-RANGE check
// refuses a field pointing outside the list, so a declaration nothing points at
// used to be invisible while protoc refuses it ("Oneof must have at least one
// field."). The coverage check sits in `resolve_message` beside that range
// check and AFTER the field loop, which is why row (d) below still sees the
// more specific index failure first.
void test_descriptor_origin_empty_oneof_is_rejected()
{
    // (a) One declaration no field points at.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "oneof.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_oneof_decl(file, holder, "pick");
        file.file.message_type.push_back(holder);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_EMPTY_ONEOF);
        CHECK(error.file == "oneof.pbs");
        CHECK(error.message.find("oneof \"pick\" in .pkg.Holder has no fields")
              == 0);
        CHECK(error.message.find(" at [") != std::string::npos);
    }
    // (b) The SECOND declaration is the unclaimed one: the first stays legal,
    // so the check is per declaration and not "some declaration is claimed".
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "tail.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_oneof_decl(file, holder, "first");
        add_oneof_decl(file, holder, "second");
        add_field(file, holder, "x", 1, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        claim_oneof(holder, 0, 0);
        file.file.message_type.push_back(holder);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_EMPTY_ONEOF);
        CHECK(error.message.find("oneof \"second\"") == 0);
    }
    // (c) Every declaration claimed stays legal - one field each - and so does
    // a message with no declaration at all, whose fields carry no index: the
    // check refuses an unclaimed declaration, not an absent oneof.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "claimed.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_oneof_decl(file, holder, "first");
        add_oneof_decl(file, holder, "second");
        add_field(file, holder, "x", 1, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        add_field(file, holder, "y", 2, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        claim_oneof(holder, 0, 0);
        claim_oneof(holder, 1, 1);
        DescriptorProto plain;
        plain.name = file.strings.save("Plain");
        plain.has_name = true;
        add_field(file, plain, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        file.file.message_type.push_back(holder);
        file.file.message_type.push_back(plain);

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    // (d) Precedence: a message that carries BOTH an unclaimed declaration and
    // a field pointing outside the list is still reported by the more specific
    // index rule first, because the coverage check runs after the field loop.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "both.pbs", "pkg");
        file.from_descriptor_set = true;
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_oneof_decl(file, holder, "pick");
        add_field(file, holder, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL, 0);
        claim_oneof(holder, 0, 4);
        file.file.message_type.push_back(holder);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
        CHECK(error.message.find("oneof_index") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Step 9 - lifetime, determinism, and both language standards
//
// Which file each check goes in is part of the step (A12): everything below
// links `easypb_schema` alone, so this whole group runs in the parser-free
// `cxx11-nodesc` profile as well. The two checks that need
// `load_source_files` - buffer destruction and the source locations that
// outlive it - live in test_source_linker.cpp.
// ---------------------------------------------------------------------------

// Decimal text without std::to_string: this file is also compiled by the
// legacy lane's GCC 4.7 / Clang 3.1-class toolchains, where that overload is a
// library-version question rather than a language one (the `D-18` class), and
// the fingerprints below only ever need digits.
std::string digits(int value)
{
    if (value == 0) return "0";
    const bool negative = value < 0;
    if (negative) value = -value;
    std::string text;
    while (value != 0) {
        text.insert(text.begin(), static_cast<char>('0' + value % 10));
        value /= 10;
    }
    if (negative) text.insert(text.begin(), '-');
    return text;
}

// A whole-view fingerprint: every file (in name order), its visible set, every
// top-level field's resolved absolute name and kind, and every entry of the
// published index - its key, its kind, and the LOGICAL NAME of the file that
// owns the entry. Files are listed by logical name and the file lines are
// sorted, because no output may depend on pointer identity (A2).
//
// The index half is what makes the WALK ORDER observable in a successful link.
// A package scope merges across files instead of colliding, and the merged node
// keeps the record of the first file that declared it (`add_symbol` only fills
// an absent key), so the owner of a package two files share is a function of the
// walk order and of nothing else. With the name-sorted walk (A2) every insertion
// order yields the same owner; an implementation that walked in insertion order
// would print a different owner per order. Measured, not argued: dropping the
// `stable_sort` in `files_by_logical_name` reddens the two order twins below
// (ledger `D-46`).
std::string view_fingerprint(const easypb_schema::SchemaSet& set,
                             const easypb_schema::LinkedSchema& linked)
{
    const std::vector<std::unique_ptr<easypb_schema::SchemaFile> >& files =
        set.files();
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string line = text(files[i]->file.name);
        line += " [";
        line += visible_names(linked, *files[i]);
        line += "]";
        for (std::size_t m = 0; m < files[i]->file.message_type.size(); ++m) {
            const DescriptorProto& message = files[i]->file.message_type[m];
            for (std::size_t f = 0; f < message.field.size(); ++f) {
                const FieldDescriptorProto& field = message.field[f];
                line += " ";
                line += text(message.name);
                line += ".";
                line += text(field.name);
                line += "=";
                line += text(field.type_name);
                line += ":";
                line += digits(field.has_type ? field.type : -1);
            }
        }
        lines.push_back(line);
    }
    std::sort(lines.begin(), lines.end());
    std::string joined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        joined += lines[i];
        joined += "\n";
    }
    // The index half: `std::map` iterates its keys in order, so this block is a
    // function of the names alone. The owner is named by LOGICAL NAME, never by
    // address.
    joined += "index:\n";
    for (std::map<std::string, easypb_schema::SymbolEntry>::const_iterator entry =
             linked.symbols.symbols.begin();
         entry != linked.symbols.symbols.end(); ++entry) {
        joined += entry->first;
        joined += "=";
        joined += digits(entry->second.kind);
        joined += "@";
        joined += entry->second.owner != 0
                      ? text(entry->second.owner->file.name)
                      : std::string("<no owner>");
        joined += "\n";
    }
    return joined;
}

// O1/O5 (Step 9): LinkedSchema is a non-owning view, and the names it writes
// are interned into the OWNING file's StringPool - a resolved field's type_name
// is `file.strings.save(...)` for the file being resolved
// (schema_semantics.cpp:1828). The pool hands out pointers into fixed blocks and
// starts a new block when the current one is full, so interning more strings
// after the link must not disturb a single published name. A view holding a
// pointer into a freed or moved block - the bug this looks for - fails here.
//
// What bites, and what does not, is measured on two defect shapes built on a
// copy of this revision (ledger `D-47`): the RE-READ TEXT of a published name
// and the re-read visibility set are what fail. The stored address and size do
// not, and could not: `data()` and `size()` return the field's own recorded
// values, which change only when something WRITES the field, so reading them
// twice is a tautology whatever the pool does - the earlier version of this test
// asserted them and neither shape fired it. The profile split is measured too:
// in C++11/14 `str_view` is std::string (descriptor.pb.hpp), the field owns a
// copy of the name, and the pool's storage is not observable through a
// descriptor at all - so this check is a lifetime check in the C++17 tree and a
// growth-path exercise in the C++11/14 ones.
void test_pool_growth_after_link_keeps_every_published_name()
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;

    easypb_schema::SchemaSet set;
    // The references are spelled RELATIVELY on purpose: the link is what interns
    // the absolute names into a.proto's pool, so the names under test are ones
    // the pool received after the fixture was built.
    easypb_schema::SchemaFile& a = make_file(
        set, "a.proto", std::vector<std::string>(1, "b.proto"),
        no_indexes, no_indexes);
    set_package(a, "app");
    // TWO published names in one file, deliberately: a pool that hands every
    // name the same buffer collapses them into one answer, and the pair of reads
    // below then disagrees with itself as well as with the expected text.
    DescriptorProto holder;
    holder.name = a.strings.save("A");
    holder.has_name = true;
    add_field(a, holder, "v", 1, FieldDescriptorProto::LABEL_OPTIONAL,
              "shared.Value");
    add_field(a, holder, "w", 2, FieldDescriptorProto::LABEL_OPTIONAL,
              "shared.Other");
    a.file.message_type.push_back(holder);
    easypb_schema::SchemaFile& b = make_file(set, "b.proto", no_deps,
                                             no_indexes, no_indexes);
    set_package(b, "shared");
    add_message(b, "Value");
    add_message(b, "Other");

    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                     linked, error));
    CHECK(linked.local_only);

    CHECK(text(a.file.message_type[0].field[0].type_name) == ".shared.Value");
    CHECK(text(a.file.message_type[0].field[1].type_name) == ".shared.Other");
    const std::string visible_before = visible_names(linked, a);
    CHECK(visible_before == "a.proto,b.proto");

    // Force the growth path in BOTH pools: the referencing file's, which owns
    // the resolved spellings, and the declaring file's, whose descriptors the
    // index entries point at. The first block is 4096 bytes, so 5000 bytes
    // always allocate another one, and the save after it lands in that new block
    // exactly as a later interning pass would.
    const std::string filler(5000, 'x');
    a.strings.save(filler);
    b.strings.save(filler);
    a.strings.save("after-growth");
    b.strings.save("after-growth");

    // The FIRST bytes a pool ever saved for a file are its logical name, and a
    // block the growth abandons is clobbered at its start (the allocator writes
    // its own free-list bookkeeping there before anything the fixture saves
    // later), so these two reads are the growth check's sharpest edge: measured
    // on the freed-blocks shape, `a.proto` is the string that comes back wrong.
    // Stated directly here, not only through `visible_names` below, so the
    // assertion that reports the shape names the string it lost.
    CHECK(text(a.file.name) == "a.proto");
    CHECK(text(b.file.name) == "b.proto");
    // Both published names, re-read after the growth: each must still read its
    // own text, and the two must not have collapsed into one buffer.
    CHECK(text(a.file.message_type[0].field[0].type_name) == ".shared.Value");
    CHECK(text(a.file.message_type[0].field[1].type_name) == ".shared.Other");
    CHECK(text(a.file.message_type[0].field[0].type_name) !=
          text(a.file.message_type[0].field[1].type_name));
    // The owned half of the view - the index keys and the visibility sets -
    // is untouched too: it stores no pointer into a pool.
    CHECK(linked.symbols.symbols.count(".shared.Value") == 1);
    CHECK(linked.symbols.symbols.count(".shared.Other") == 1);
    CHECK(visible_names(linked, a) == visible_before);
}

// O2/O6 (Step 9): index and visibility entries borrow `SchemaFile*`, and files
// live on the heap, so a pointer taken before the owning vector grew still
// names the same file afterwards. Growth is the normal case - a set is built
// file by file - so what is pinned here is that a link performed AFTER many
// more files were added still publishes the same pointers for the files that
// were already there, and that the view published earlier still reads through
// them.
void test_set_growth_keeps_file_addresses_and_the_published_view()
{
    easypb_schema::SchemaSet set;
    make_chain_fixture(set, false, false);

    easypb_schema::SchemaFile* const a_before = set.find_file("a.proto");
    easypb_schema::SchemaFile* const b_before = set.find_file("b.proto");
    CHECK(a_before != 0 && b_before != 0);

    easypb_schema::LinkedSchema first;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                     first, error));
    CHECK(visible_names(first, *a_before) == "a.proto,b.proto");

    // Now grow the set well past several reallocations of the owning vector
    // and rehashes of the name index. Each filler carries its own message, so
    // the index has real work to do and no two declarations collide.
    std::vector<easypb_schema::SchemaFile*> fillers;
    for (int i = 0; i < 32; ++i) {
        const std::string suffix = digits(i);
        const std::string file_name = "filler" + suffix + ".proto";
        const std::string message_name = "F" + suffix;
        easypb_schema::SchemaFile& filler =
            make_simple_file(set, file_name.c_str(), 0);
        add_message(filler, message_name.c_str());
        fillers.push_back(&filler);
    }

    // The same objects are still reachable by name: the vector moved its
    // unique_ptr elements, never the files they own.
    CHECK(set.find_file("a.proto") == a_before);
    CHECK(set.find_file("b.proto") == b_before);
    CHECK(set.find_file("filler0.proto") == fillers[0]);
    CHECK(set.find_file("filler31.proto") == fillers[31]);

    // The view published BEFORE the growth still reads through the recorded
    // pointers: if the addresses had moved, this would dereference a stale
    // file and the names would not come back.
    CHECK(first.visible_files.count(a_before) == 1);
    CHECK(visible_names(first, *a_before) == "a.proto,b.proto");
    CHECK(visible_names(first, *b_before) == "b.proto,c.proto");

    // A fresh link after the growth publishes the same pointers for the old
    // files and one entry per new one.
    easypb_schema::LinkedSchema second;
    easypb_schema::Diagnostic second_error;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                     second, second_error));
    CHECK(second.visible_files.count(a_before) == 1);
    CHECK(visible_names(second, *a_before) == "a.proto,b.proto");
    for (std::size_t i = 0; i < fillers.size(); ++i) {
        CHECK(second.visible_files.count(fillers[i]) == 1);
        CHECK(visible_names(second, *fillers[i]) ==
              text(fillers[i]->file.name));
    }
}

// O4/R21 + Step 5.1 (Step 9): the descriptor-only half of the isolation
// contract. This suite links `easypb_schema` and nothing else, which is the
// structural half (a `cxx11-nodesc` run); the diagnostic half is what the
// cases below pin. A descriptor-origin file has no FieldSource at all and no
// physical identity, so a missing reference must be reported from its
// descriptor path and a missing dependency from its logical name - never by
// looking for a source position or falling back to a source read.
void test_descriptor_only_isolation_reports_a_descriptor_path()
{
    // (a) an unresolved reference: logical file plus the field's descriptor
    // path, with the file carrying no provenance and no physical name.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "iso.pbs", "pkg");
        file.from_descriptor_set = true;
        file.physical_name.clear();
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_field(file, holder, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  "shared.Value");
        file.file.message_type.push_back(holder);

        // Step 5.1's shape, asserted rather than assumed: no FieldSource
        // exists, so nothing in the failure path may depend on one.
        CHECK(file.field_sources.empty());
        CHECK(file.physical_name.empty());

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                          linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
        CHECK(error.file == "iso.pbs");
        CHECK(error.message.find("at [4, 0, 2, 0]") != std::string::npos);
        CHECK(linked.files == 0);
        CHECK(!linked.local_only);
    }
    // (b) a declared dependency the set does not contain: an in-memory graph
    // error naming the importing logical file and the missing logical import,
    // with nothing to read from disk even if a source fallback were attempted.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = make_file(
            set, "iso.pbs", std::vector<std::string>(1, "absent.proto"),
            std::vector<std::int32_t>(), std::vector<std::int32_t>());
        file.from_descriptor_set = true;
        file.physical_name.clear();
        add_message(file, "Holder");

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                          linked, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
        CHECK(error.file == "iso.pbs");
        CHECK(error.message.find("absent.proto") != std::string::npos);
        CHECK(linked.files == 0);
    }
    // (c) the control: the very same descriptor-only shape links once the
    // definition is in the set, so (a) is about the missing definition and not
    // about the descriptor origin.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "iso.pbs", "pkg");
        file.from_descriptor_set = true;
        file.physical_name.clear();
        DescriptorProto holder;
        holder.name = file.strings.save("Holder");
        holder.has_name = true;
        add_field(file, holder, "f", 1, FieldDescriptorProto::LABEL_OPTIONAL,
                  "shared.Value");
        file.file.message_type.push_back(holder);

        easypb_schema::SchemaFile& dep = make_simple_file(set, "dep.pbs", "shared");
        dep.from_descriptor_set = true;
        dep.physical_name.clear();
        add_message(dep, "Value");
        // The reference is absolute, and the dependency index is what makes
        // dep.pbs visible at all.
        file.file.dependency.push_back(file.strings.save("dep.pbs"));
        file.file.public_dependency.push_back(0);

        easypb_schema::Diagnostic error;
        CHECK(link_verdict(set, error));
        CHECK(text(file.file.message_type[0].field[0].type_name) ==
              ".shared.Value");
    }
}

// The repeated-run fixture: root.proto (app) references deep.Leaf, which
// mid.proto (deep) declares; mid.proto public-imports leaf.proto, which gives
// every file a different visible set and gives the fingerprint something to
// compare. `reverse` builds the identical set in the opposite insertion order.
void make_repeat_fixture(easypb_schema::SchemaSet& set, bool reverse)
{
    const std::vector<std::string> no_deps;
    const std::vector<std::int32_t> no_indexes;
    const std::vector<std::int32_t> index_zero = repeated_indexes(1, 0);

    for (int pass = 0; pass < 3; ++pass) {
        const int which = reverse ? 2 - pass : pass;
        if (which == 0) {
            easypb_schema::SchemaFile& root = make_file(
                set, "root.proto", std::vector<std::string>(1, "mid.proto"),
                no_indexes, no_indexes);
            set_package(root, "app");
            add_message_with_field(root, "Root", "v", 1, "deep.Leaf");
        } else if (which == 1) {
            easypb_schema::SchemaFile& mid = make_file(
                set, "mid.proto", std::vector<std::string>(1, "leaf.proto"),
                index_zero, no_indexes);
            set_package(mid, "deep");
            add_enum(mid, "Leaf", "A");
        } else {
            easypb_schema::SchemaFile& leaf = make_file(
                set, "leaf.proto", no_deps, no_indexes, no_indexes);
            set_package(leaf, "deep");
            add_message(leaf, "Other");
        }
    }
}

// O6/A2 (Step 9): the same set linked twice, each time into a fresh
// LinkedSchema, must publish an identical view - the same resolved names and
// kinds, the same visibility sets, and the same index, entry for entry. The
// second run resolves an input whose type_names the first run already rewrote
// to their absolute spelling, which test_already_resolved_input_is_resolved_again
// pins separately; here the question is determinism, not the reference
// spelling. The twin builds the same fixture in the reverse insertion order,
// and the whole-view fingerprint includes the OWNER of every index entry: a
// package scope declared by two files keeps the first declarer it saw, which is
// where the walk order becomes observable in a successful link. This fixture's
// `deep` package is declared by both mid.proto and leaf.proto, so an
// insertion-ordered walk would record mid.proto for one insertion order and
// leaf.proto for the other and the comparison below would fail; the name-sorted
// walk (A2) gives leaf.proto in both. The first-reported-failure half of the
// same rule is Step 5.2's
// test_input_order_and_targets_do_not_decide_the_verdict, which pins the
// verdict and the resolved names across six permutations of a three-file set - so
// that half is inherited rather than repeated here. `D-46`'s measurement:
// dropping the name-sorted walk reddens the assertions below.
void test_repeated_runs_publish_identical_views()
{
    {
        easypb_schema::SchemaSet set;
        make_repeat_fixture(set, false);

        easypb_schema::LinkedSchema first;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                         first, error));
        CHECK(first.files == &set);
        CHECK(first.local_only);
        const std::string first_view = view_fingerprint(set, first);
        // Lines are sorted by logical name, so the root's line is the last
        // one; what matters is that it names the resolved absolute type and
        // its kind (TYPE_ENUM == 14), with the transitively public leaf in
        // the visible set.
        CHECK(first_view.find("root.proto [leaf.proto,mid.proto,root.proto] "
                              "Root.v=.deep.Leaf:14") != std::string::npos);

        easypb_schema::LinkedSchema second;
        easypb_schema::Diagnostic second_error;
        CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                         second, second_error));
        CHECK(second.files == &set);
        CHECK(second.local_only);
        CHECK(view_fingerprint(set, second) == first_view);
        // The visibility sets are compared directly too, key for key: the
        // fingerprints above compare the names they resolve to, this compares
        // the entries themselves.
        CHECK(second.visible_files == first.visible_files);
        CHECK(second.symbols.symbols.size() == first.symbols.symbols.size());
    }
    {
        easypb_schema::SchemaSet forward_set;
        make_repeat_fixture(forward_set, false);
        easypb_schema::LinkedSchema forward;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::link_schema(forward_set, easypb_schema::LINK_COMPLETE,
                                         forward, error));

        easypb_schema::SchemaSet backward_set;
        make_repeat_fixture(backward_set, true);
        easypb_schema::LinkedSchema backward;
        easypb_schema::Diagnostic backward_error;
        CHECK(easypb_schema::link_schema(backward_set,
                                         easypb_schema::LINK_COMPLETE,
                                         backward, backward_error));
        CHECK(backward.local_only);
        CHECK(view_fingerprint(backward_set, backward) ==
              view_fingerprint(forward_set, forward));

        // The order claim, asserted rather than implied: `.deep` is declared by
        // mid.proto and leaf.proto, so the merged package node keeps whichever
        // declarer the walk reached first - leaf.proto under the name-sorted
        // walk (A2), in BOTH insertion orders, and mid.proto for the forward
        // one under an insertion-ordered walk. The whole-view comparison above
        // is what catches that; the pair here makes the observable explicit, so
        // the comparison cannot go vacuous if the fingerprint ever stops
        // carrying the owner.
        const std::map<std::string, easypb_schema::SymbolEntry>::const_iterator
            forward_deep = forward.symbols.symbols.find(".deep");
        CHECK(forward_deep != forward.symbols.symbols.end());
        if (forward_deep != forward.symbols.symbols.end()) {
            CHECK(text(forward_deep->second.owner->file.name) == "leaf.proto");
        }
        const std::map<std::string, easypb_schema::SymbolEntry>::const_iterator
            backward_deep = backward.symbols.symbols.find(".deep");
        CHECK(backward_deep != backward.symbols.symbols.end());
        if (backward_deep != backward.symbols.symbols.end()) {
            CHECK(text(backward_deep->second.owner->file.name) == "leaf.proto");
        }
    }
}

// A2/R5 (Step 9): for one name with three candidate kinds along the walk - a
// nearer NON-TYPE head, a nearer head whose owner is invisible, and the visible
// aggregate that actually owns the member - the answer must be the same
// whichever order the three files were added. The rule pair itself is pinned by
// test_non_type_heads_do_not_shadow (a field head) and
// test_invisible_candidates_do_not_shadow (the B10/B11 message head, whose
// second half makes the hidden file a dependency and expects the refusal); what
// this adds is that no insertion order can select a different candidate.
//
// The loop is also the delta's discriminating order check (`D-46`): `pkg` is
// declared by BOTH a.proto and hid.proto, and the whole-view fingerprint carries
// each index entry's owner, so the merged `.pkg` node records the name-sorted
// first declarer whatever the insertion order - while an insertion-ordered walk
// records hid.proto for the three orders that insert it before a.proto and the
// per-order assertions plus the fingerprint comparison go red. Measured by
// dropping `files_by_logical_name`'s `stable_sort`.
//
// The requester is a.proto (package pkg, message M importing vis.proto):
// `sub` is a FIELD inside M (non-type, nearest), `.pkg.sub` is a MESSAGE owned
// by hid.proto, which a.proto does not import (invisible), and `.sub.X` is the
// message vis.proto declares under package `sub` (visible).
void test_iterator_rules_stability_across_file_order()
{
    const char* const orders[6][3] = {
        { "a.proto", "vis.proto", "hid.proto" },
        { "a.proto", "hid.proto", "vis.proto" },
        { "vis.proto", "a.proto", "hid.proto" },
        { "vis.proto", "hid.proto", "a.proto" },
        { "hid.proto", "a.proto", "vis.proto" },
        { "hid.proto", "vis.proto", "a.proto" }
    };

    std::string first_view;
    for (int order = 0; order < 6; ++order) {
        const std::vector<std::string> no_deps;
        const std::vector<std::int32_t> no_indexes;
        easypb_schema::SchemaSet set;
        for (int slot = 0; slot < 3; ++slot) {
            const std::string name = orders[order][slot];
            if (name == "a.proto") {
                easypb_schema::SchemaFile& a = make_file(
                    set, "a.proto", std::vector<std::string>(1, "vis.proto"),
                    no_indexes, no_indexes);
                set_package(a, "pkg");
                DescriptorProto m;
                m.name = a.strings.save("M");
                m.has_name = true;
                add_field(a, m, "sub", 1, FieldDescriptorProto::LABEL_OPTIONAL, 0);
                add_field(a, m, "f", 2, FieldDescriptorProto::LABEL_OPTIONAL,
                          "sub.X");
                a.file.message_type.push_back(m);
            } else if (name == "vis.proto") {
                easypb_schema::SchemaFile& vis =
                    make_file(set, "vis.proto", no_deps, no_indexes, no_indexes);
                set_package(vis, "sub");
                add_message(vis, "X");
            } else {
                easypb_schema::SchemaFile& hid =
                    make_file(set, "hid.proto", no_deps, no_indexes, no_indexes);
                set_package(hid, "pkg");
                add_message(hid, "sub");
            }
        }

        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                         linked, error));
        CHECK(linked.local_only);
        const easypb_schema::SchemaFile* const a = set.find_file("a.proto");
        CHECK(a != 0);
        CHECK(text(a->file.message_type[0].field[1].type_name) == ".sub.X");
        CHECK(a->file.message_type[0].field[1].type ==
              FieldDescriptorProto::TYPE_MESSAGE);
        CHECK(visible_names(linked, *a) == "a.proto,vis.proto");

        // The walk order's observable for this fixture, asserted per order:
        // `.pkg` is declared by a.proto and by hid.proto, the merged node keeps
        // the first declarer it saw, and the walk is name-sorted, so it is
        // a.proto in every one of the six orders. An insertion-ordered walk
        // names hid.proto whenever hid.proto came first.
        const std::map<std::string, easypb_schema::SymbolEntry>::const_iterator pkg =
            linked.symbols.symbols.find(".pkg");
        CHECK(pkg != linked.symbols.symbols.end());
        if (pkg != linked.symbols.symbols.end()) {
            CHECK(text(pkg->second.owner->file.name) == "a.proto");
        }

        const std::string view = view_fingerprint(set, linked);
        if (order == 0) {
            first_view = view;
        } else {
            CHECK(view == first_view);
        }
    }
}

} // namespace

// Records a `[packed = ...]` option on a field, the way the parser stores it:
// presence and value are separate questions, which is the whole subject of
// D-51.
void set_field_packed(FieldDescriptorProto& field, bool packed)
{
    field.options.packed = packed;
    field.options.has_packed = true;
    field.has_options = true;
}

// A reference to the service just appended, so a method can be declared in it.
ServiceDescriptorProto& add_service_named(
    easypb_schema::SchemaFile& file, const char* name)
{
    ServiceDescriptorProto service;
    service.name = file.strings.save(name);
    service.has_name = true;
    file.file.service.push_back(service);
    return file.file.service.back();
}

void add_method(easypb_schema::SchemaFile& file,
                ServiceDescriptorProto& service, const char* name,
                const char* input_type = 0, const char* output_type = 0)
{
    MethodDescriptorProto method;
    method.name = file.strings.save(name);
    method.has_name = true;
    if (input_type != 0) {
        method.input_type = file.strings.save(input_type);
        method.has_input_type = true;
    }
    if (output_type != 0) {
        method.output_type = file.strings.save(output_type);
        method.has_output_type = true;
    }
    service.method.push_back(method);
}

// D-52 at the link's entry point: the edition field decides the class before
// the syntax string, so all three measured shapes get protoc's classification
// instead of the pre-fix "read the string" answer.
void test_descriptor_origin_editions_field_decides_the_class()
{
    // (a) edition = EDITION_PROTO3 with a `required` field: the file is proto3,
    // so clause 12's proto3 rule fires.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "edition3.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.has_edition = true;
        file.file.edition = easypb_schema::kEditionProto3;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "a", 1,
                  FieldDescriptorProto::LABEL_REQUIRED, 0);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_PROTO3_REQUIRED_FIELD);
    }
    // (b) edition = EDITION_PROTO2 beside `syntax = "proto3"`: the edition
    // wins, so the required field is legal (protoc's `has_edition()` order).
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "edition2.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.has_edition = true;
        file.file.edition = easypb_schema::kEditionProto2;
        file.file.syntax = file.strings.save("proto3");
        file.file.has_syntax = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "a", 1,
                  FieldDescriptorProto::LABEL_REQUIRED, 0);

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    // (c) `syntax = ""` with no edition is proto2, so the same required field
    // lives where it used to be refused as unsupported syntax.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "empty-syntax.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("");
        file.file.has_syntax = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "a", 1,
                  FieldDescriptorProto::LABEL_REQUIRED, 0);

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    // (d) EDITION_2023 is still refused loudly, by the A25 stage rather than by
    // a classifier that read the file as proto2.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "edition2023.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.has_edition = true;
        file.file.edition = 1000;
        add_message(file, "M");

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
    }
}

// D-61: A25's refusal names the value that DECIDED the class. The classifier
// consults `has_edition()` first, so a crafted file carrying an unsupported
// edition and no syntax string was described as `declares syntax ""` - a string
// it does not have - and one carrying `syntax = "proto3"` beside an unsupported
// edition was described as unsupported SYNTAX while "proto3" is listed as
// accepted in the same sentence. The two sentences are pinned here; the string
// sentence stays exactly as A25 wrote it for the files the string decided.
void test_unsupported_syntax_refusal_names_what_decided()
{
    // (a) the edition field decided, and protobuf 30.1 has a year for it.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "edition2023.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.has_edition = true;
        file.file.edition = easypb_schema::kEdition2023;
        add_message(file, "M");

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
        CHECK(error.message ==
              "file \"edition2023.pbs\" declares edition 2023, which is not "
              "supported: only absent, \"proto2\", and \"proto3\" are "
              "accepted");
    }
    // (b) the same file also carries `syntax = "proto3"`: the edition decided,
    // so the sentence names the edition and does not quote the string.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "edition2023-proto3.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.has_edition = true;
        file.file.edition = easypb_schema::kEdition2023;
        file.file.syntax = file.strings.save("proto3");
        file.file.has_syntax = true;
        add_message(file, "M");

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.message ==
              "file \"edition2023-proto3.pbs\" declares edition 2023, which "
              "is not supported: only absent, \"proto2\", and \"proto3\" "
              "are accepted");
        CHECK(error.message.find("declares syntax") == std::string::npos);
    }
    // (c) an edition protobuf 30.1 has no year for is named by its number: the
    // sentence reports what was read instead of inventing a name for it.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "edition1002.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.has_edition = true;
        file.file.edition = 1002;
        add_message(file, "M");

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.message.find("declares edition 1002") !=
              std::string::npos);
        CHECK(error.message.find("declares syntax") == std::string::npos);
    }
    // (d) the STRING decided: no edition field, so the sentence is A25's own,
    // with the string that was read quoted.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "editions-string.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.syntax = file.strings.save("editions");
        file.file.has_syntax = true;
        add_message(file, "M");

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX);
        CHECK(error.message ==
              "file \"editions-string.pbs\" declares syntax \"editions\", "
              "which is not supported: only absent, \"proto2\", and "
              "\"proto3\" are accepted");
    }
    // (e) control: the two editions this model DOES read never reach the
    // sentence, so no refusal may name them.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "edition2.pbs", "pkg");
        file.from_descriptor_set = true;
        file.file.has_edition = true;
        file.file.edition = easypb_schema::kEditionProto2;
        add_message(file, "M");

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
}

// D-51: `packed` is gated on its VALUE, not on presence. protoc refuses only
// `[packed = true]` on a non-packable field and accepts `[packed = false]`
// anywhere, writing `has_packed = true, packed = false`; a presence-only gate
// refused the sets protoc itself writes.
void test_descriptor_origin_packed_false_is_accepted()
{
    // (a) `[packed = false]` on an optional field.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "packed-false-optional.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "a", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        set_field_packed(file.file.message_type[0].field[0], false);

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    // (b) `[packed = false]` on a repeated string.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "packed-false-string.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "s", 1,
                  FieldDescriptorProto::LABEL_REPEATED, 0);
        file.file.message_type[0].field[0].type =
            FieldDescriptorProto::TYPE_STRING;
        set_field_packed(file.file.message_type[0].field[0], false);

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    // (c) `[packed = false]` on a repeated message.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "packed-false-message.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        DescriptorProto& message = file.file.message_type[0];
        add_nested(file, message, "N");
        add_field(file, message, "n", 1, FieldDescriptorProto::LABEL_REPEATED,
                  ".pkg.M.N");
        set_field_packed(message.field[0], false);

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    // (d) Control: `[packed = true]` on a repeated string is still refused.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "packed-true-string.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "s", 1,
                  FieldDescriptorProto::LABEL_REPEATED, 0);
        file.file.message_type[0].field[0].type =
            FieldDescriptorProto::TYPE_STRING;
        set_field_packed(file.file.message_type[0].field[0], true);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.message.find("packed") != std::string::npos);
    }
}

// D-53 (fix round r25) and D-62 (fix round r26): A26's sweep asks the clause
// groups of protoc's `ValidateMapEntry` that a descriptor alone can answer, not
// just the two field numbers. Each row mutates one member of a clean entry and
// requires the dedicated code in both modes; the controls at the end keep the
// rule from refusing every entry.
enum MapEntryShape {
    MAP_SHAPE_BYTES_KEY,
    MAP_SHAPE_FLOAT_KEY,
    MAP_SHAPE_ENUM_KEY,
    MAP_SHAPE_THIRD_FIELD,
    MAP_SHAPE_KEY_RENAMED,
    MAP_SHAPE_VALUE_RENAMED,
    MAP_SHAPE_REPEATED_KEY,
    MAP_SHAPE_NESTED_TYPE,
    MAP_SHAPE_NESTED_ENUM,
    MAP_SHAPE_EXTENSION_RANGE,
    MAP_SHAPE_EXTENSION_MEMBER
};

void build_map_entry_shape(easypb_schema::SchemaFile& file,
                           DescriptorProto& owner, MapEntryShape shape)
{
    add_map_entry(file, owner, "ItemsEntry", 0);
    DescriptorProto& entry = owner.nested_type[0];
    switch (shape) {
        case MAP_SHAPE_BYTES_KEY:
            entry.field[0].type = FieldDescriptorProto::TYPE_BYTES;
            break;
        case MAP_SHAPE_FLOAT_KEY:
            entry.field[0].type = FieldDescriptorProto::TYPE_FLOAT;
            break;
        case MAP_SHAPE_ENUM_KEY:
            entry.field[0].type = FieldDescriptorProto::TYPE_ENUM;
            break;
        case MAP_SHAPE_THIRD_FIELD: {
            FieldDescriptorProto extra;
            extra.name = file.strings.save("extra");
            extra.has_name = true;
            extra.number = 3;
            extra.has_number = true;
            extra.label = FieldDescriptorProto::LABEL_OPTIONAL;
            extra.has_label = true;
            extra.type = FieldDescriptorProto::TYPE_INT32;
            extra.has_type = true;
            entry.field.push_back(extra);
            break;
        }
        case MAP_SHAPE_KEY_RENAMED:
            entry.field[0].name = file.strings.save("k");
            break;
        case MAP_SHAPE_VALUE_RENAMED:
            entry.field[1].name = file.strings.save("v");
            break;
        case MAP_SHAPE_REPEATED_KEY:
            entry.field[0].label = FieldDescriptorProto::LABEL_REPEATED;
            break;
        case MAP_SHAPE_NESTED_TYPE:
            add_nested(file, entry, "N");
            break;
        case MAP_SHAPE_NESTED_ENUM: {
            EnumDescriptorProto nested_enum;
            nested_enum.name = file.strings.save("E");
            nested_enum.has_name = true;
            EnumValueDescriptorProto value;
            value.name = file.strings.save("ZERO");
            value.has_name = true;
            value.number = 0;
            value.has_number = true;
            nested_enum.value.push_back(value);
            entry.enum_type.push_back(nested_enum);
            break;
        }
        case MAP_SHAPE_EXTENSION_RANGE:
            // D-62: wire field 5 is modelled as a range list since W-45, so the
            // shape carries one range; the sweep refuses its PRESENCE, which is
            // all the map-entry rule asks. The decode-level twin - that the
            // range survives the decoder with its numbers - is
            // test_map_entry_extension_data_is_counted_and_refused.
            {
                DescriptorProto_ExtensionRange range;
                range.start = 10;
                range.end = 20;
                range.has_start = true;
                range.has_end = true;
                entry.extension_range.push_back(range);
            }
            break;
        case MAP_SHAPE_EXTENSION_MEMBER:
            entry.extension_count = 1;
            break;
    }
}

void test_malformed_map_entry_shape_is_rejected()
{
    const MapEntryShape shapes[] = {
        MAP_SHAPE_BYTES_KEY,   MAP_SHAPE_FLOAT_KEY,    MAP_SHAPE_ENUM_KEY,
        MAP_SHAPE_THIRD_FIELD, MAP_SHAPE_KEY_RENAMED,  MAP_SHAPE_VALUE_RENAMED,
        MAP_SHAPE_REPEATED_KEY, MAP_SHAPE_NESTED_TYPE, MAP_SHAPE_NESTED_ENUM,
        MAP_SHAPE_EXTENSION_RANGE, MAP_SHAPE_EXTENSION_MEMBER
    };
    for (std::size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); ++i) {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "map_shape.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        build_map_entry_shape(file, file.file.message_type[0], shapes[i]);

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MALFORMED_MAP_ENTRY);
        CHECK(error.message.find("Malformed map entry") != std::string::npos);
    }
    // Control: a clean entry links, referenced or not, so the extended sweep
    // cannot pass by refusing every entry.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "map_shape.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        add_map_entry(file, file.file.message_type[0], "ItemsEntry", 0);

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
}

// D-62: a synthetic entry's extension data is COUNTED by the decoder, and the
// sweep refuses what the count shows. The bytes below are the review round's
// own crafted counter-example (`map_entry_extension_range`), rebuilt here for
// the suite: `DescriptorProto` carries no extension member at all before the
// fix, so a model-level row alone would leave the decoder's half unproven - a
// counter that never left zero makes the sweep unreachable while every other
// row still passes.
//
// The encoder is hand-written rather than the library's Writer so the bytes are
// an independent input: a helper shared with the decoder could cancel a wrong
// field number out. Field numbers and enum values are descriptor.proto's own
// (LABEL_OPTIONAL = 1, LABEL_REPEATED = 3, TYPE_INT32 = 5, TYPE_MESSAGE = 11,
// DescriptorProto.extension_range = 5, .extension = 6, .options = 7,
// FileDescriptorProto.message_type = 4).
void wire_varint(std::string& out, std::uint64_t value)
{
    while (value >= 0x80) {
        out.push_back(char((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(char(value));
}

std::string wire_tag(int field, unsigned wire)
{
    std::string out;
    wire_varint(out, (std::uint64_t(field) << 3) | wire);
    return out;
}

std::string wire_varint_field(int field, std::uint64_t value)
{
    std::string out = wire_tag(field, 0);
    wire_varint(out, value);
    return out;
}

std::string wire_text_field(int field, const char* text)
{
    const std::string payload(text);
    std::string out = wire_tag(field, 2);
    wire_varint(out, payload.size());
    out += payload;
    return out;
}

std::string wire_message_field(int field, const std::string& payload)
{
    std::string out = wire_tag(field, 2);
    wire_varint(out, payload.size());
    out += payload;
    return out;
}

// One FieldDescriptorProto: name, extendee, number, label, type, type_name.
// `type_name` is field 6 and `extendee` field 2 - the two are different wire
// fields, which is why they are two parameters.
std::string wire_field(const char* name, int number, int label, int type,
                       const char* type_name, const char* extendee = 0)
{
    std::string out = wire_text_field(1, name);
    if (extendee != 0) out += wire_text_field(2, extendee);
    out += wire_varint_field(3, std::uint64_t(number));
    out += wire_varint_field(4, std::uint64_t(label));
    out += wire_varint_field(5, std::uint64_t(type));
    if (type_name != 0) out += wire_text_field(6, type_name);
    return out;
}

// `p.BrokenMap` with a `repeated ItemsEntry items = 1` field, the synthetic
// `ItemsEntry` that field implies, and the extension member `shape` selects:
// 0 none (control), 1 an extension range, 2 an extension field.
std::string map_entry_extension_set(int shape)
{
    std::string entry = wire_text_field(1, "ItemsEntry");
    entry += wire_message_field(2, wire_field("key", 1, 1, 5, 0));
    entry += wire_message_field(2, wire_field("value", 2, 1, 5, 0));
    if (shape == 1) {
        std::string range = wire_varint_field(1, 10);   // start
        range += wire_varint_field(2, 20);              // end (exclusive)
        entry += wire_message_field(5, range);
    } else if (shape == 2) {
        entry += wire_message_field(
            6, wire_field("e", 100, 1, 5, 0, ".p.BrokenMap"));
    }
    entry += wire_message_field(7, wire_varint_field(7, 1));  // map_entry

    std::string owner = wire_text_field(1, "BrokenMap");
    owner += wire_message_field(
        2, wire_field("items", 1, 3, 11, ".p.BrokenMap.ItemsEntry"));
    owner += wire_message_field(3, entry);

    std::string file = wire_text_field(1, "root.proto");
    file += wire_text_field(2, "p");
    file += wire_message_field(4, owner);
    return wire_message_field(1, file);  // the FileDescriptorSet wrapper
}

void test_map_entry_extension_data_is_counted_and_refused()
{
    for (int shape = 0; shape <= 2; ++shape) {
        const std::string bytes = map_entry_extension_set(shape);
        const FileDescriptorSet decoded =
            easypb::decode<FileDescriptorSet>(bytes);
        CHECK(decoded.file.size() == 1);
        if (decoded.file.size() != 1) return;
        CHECK(decoded.file[0].message_type.size() == 1);
        if (decoded.file[0].message_type.size() != 1) return;
        const DescriptorProto& entry =
            decoded.file[0].message_type[0].nested_type[0];
        // The decoder's own read of wire fields 5 and 6 - the sweep asks the
        // range list and the extension count.
        CHECK(entry.extension_range.size() == (shape == 1 ? 1u : 0u));
        CHECK(entry.extension_count == (shape == 2 ? 1 : 0));
        if (shape == 1 && entry.extension_range.size() == 1) {
            CHECK(entry.extension_range[0].start == 10);
            CHECK(entry.extension_range[0].end == 20);
            CHECK(entry.extension_range[0].has_start);
            CHECK(entry.extension_range[0].has_end);
        }

        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file = set.add_file("root.proto");
        file.physical_name.clear();
        file.from_descriptor_set = true;
        // The decoded views point into `bytes`, which outlives the link below.
        file.file = decoded.file[0];

        easypb_schema::Diagnostic error;
        if (shape == 0) {
            // Control: the same bytes with no extension member link in both
            // modes, so the rule cannot pass by refusing every entry.
            CHECK(accepted_in_both_modes(set, error));
        } else {
            CHECK(refused_in_both_modes(set, error));
            CHECK(error.code == easypb_schema::DIAGNOSTIC_MALFORMED_MAP_ENTRY);
            CHECK(error.message.find("Malformed map entry") !=
                  std::string::npos);
            CHECK(error.message.find("extension") != std::string::npos);
        }
    }
    // Neighbour: a message that is NOT a map entry may declare both members as
    // far as THIS rule is concerned. The rule rides the `map_entry` marker, so
    // the counters cannot refuse a plain proto2 message's extension
    // declarations. (Whether an extension declaration's NUMBER lies in a
    // declared extension range is a different check protoc makes and this
    // linker does not - extension metadata stays out of scope, clause 12 - so
    // the assertion here is about this rule alone.)
    {
        std::string owner = wire_text_field(1, "Plain");
        owner += wire_message_field(2, wire_field("a", 1, 1, 5, 0));
        std::string range = wire_varint_field(1, 10);
        range += wire_varint_field(2, 20);
        owner += wire_message_field(5, range);
        owner += wire_message_field(
            6, wire_field("e", 100, 1, 5, 0, ".p.Plain"));

        std::string file = wire_text_field(1, "plain.proto");
        file += wire_text_field(2, "p");
        file += wire_message_field(4, owner);
        const std::string bytes = wire_message_field(1, file);

        const FileDescriptorSet decoded =
            easypb::decode<FileDescriptorSet>(bytes);
        CHECK(decoded.file.size() == 1);
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& target = set.add_file("plain.proto");
        target.physical_name.clear();
        target.from_descriptor_set = true;
        target.file = decoded.file[0];

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
}

// `D-65` (fix round r28): a message declaring two nested map entries of the SAME
// name is one nested type declared twice, and the linker refuses it in both
// modes as a name collision (`DIAGNOSTIC_DUPLICATE_SYMBOL`). Both entries are
// LEGAL map entries (string/int32, then int64/string), so the refusal cannot be
// credited to the A26 key-type clause - which is what a crafted set with a
// `bytes` key answers first, and why review round r28 read the shape as refused
// for a different reason. The controls in the same test show what the rule is
// about: the same two entry shapes under distinct names link, and the same entry
// name in two different message scopes links.
//
// Why it is pinned here at all: the counter-example was found on the GENERATOR
// path, where `collect_map_types` kept the FIRST of the two same-named entries
// (`result.insert`) while the previous revision kept the LAST (`operator[]`), so
// the same bytes generated different C++ (`put_map_string_int32` against
// `put_map_bytes_string`). That path is the CLI, which never links, so the
// generator's half of the remedy - refuse the shape instead of silently picking
// one of the two entries - is asserted in `codegen.maps`
// (`tests/codegen/maps/data/duplicate_map_entry.pbs`). This test is the
// LINKER's half: the declaration-level refusal the generator's remedy rests on,
// measured with both entries legal.
// One `DescriptorProto.nested_type` (field 3 of a message): a synthetic map
// entry named `name` whose key and value are the two given kinds.
std::string wire_nested_map_entry(const char* name, int key_type, int value_type)
{
    std::string entry = wire_text_field(1, name);
    entry += wire_message_field(2, wire_field("key", 1, 1, key_type, 0));
    entry += wire_message_field(2, wire_field("value", 2, 1, value_type, 0));
    entry += wire_message_field(7, wire_varint_field(7, 1));  // map_entry
    return wire_message_field(3, entry);
}

// `p.Holder` with the given nested declarations and fields, wrapped as the
// one-file descriptor set the linker is handed.
std::string wire_holder_set(const std::string& nested, const std::string& fields,
                            const char* file_name)
{
    std::string owner = wire_text_field(1, "Holder");
    owner += fields;
    owner += nested;
    std::string file = wire_text_field(1, file_name);
    file += wire_text_field(2, "p");
    file += wire_message_field(4, owner);
    return wire_message_field(1, file);
}

void test_duplicate_map_entry_name_is_a_name_collision()
{
    // The shape: two `ItemsEntry` in one scope, both legal map entries.
    {
        const std::string bytes = wire_holder_set(
            wire_nested_map_entry("ItemsEntry", 9, 5) +  // key string, value int32
                wire_nested_map_entry("ItemsEntry", 3, 9),  // key int64, value string
            wire_message_field(
                2, wire_field("items", 1, 3, 11, ".p.Holder.ItemsEntry")),
            "duplicate_map_entry.proto");
        const FileDescriptorSet decoded =
            easypb::decode<FileDescriptorSet>(bytes);
        CHECK(decoded.file.size() == 1);
        if (decoded.file.size() != 1) return;
        CHECK(decoded.file[0].message_type.size() == 1);
        if (decoded.file[0].message_type.size() != 1) return;
        const DescriptorProto& holder = decoded.file[0].message_type[0];
        CHECK(holder.nested_type.size() == 2);
        if (holder.nested_type.size() != 2) return;
        // Both same-named declarations, both marked as map entries, and the
        // two key kinds are LEGAL (string then int64): the A26 key-type clause
        // has nothing to say about this set.
        CHECK(holder.nested_type[0].name == "ItemsEntry");
        CHECK(holder.nested_type[1].name == "ItemsEntry");
        CHECK(holder.nested_type[0].options.map_entry);
        CHECK(holder.nested_type[1].options.map_entry);
        CHECK(holder.nested_type[0].field[0].type ==
              FieldDescriptorProto::TYPE_STRING);
        CHECK(holder.nested_type[1].field[0].type ==
              FieldDescriptorProto::TYPE_INT64);

        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& seeded =
            set.add_file(std::string(decoded.file[0].name.data(),
                                     decoded.file[0].name.size()));
        seeded.physical_name.clear();
        seeded.from_descriptor_set = true;
        // The decoded views point into `bytes`, which outlives the link below.
        seeded.file = decoded.file[0];

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
        // The sentence names the colliding declaration:
        // `name collision in .p.Holder: nested message ItemsEntry conflicts
        // with nested message at [4, 0, 3, 1]`.
        CHECK(error.message.find("name collision") != std::string::npos);
        CHECK(error.message.find("ItemsEntry") != std::string::npos);
        CHECK(error.message.find("conflict") != std::string::npos);
    }

    // Control 1: the same two entry SHAPES under distinct names link in both
    // modes, so the refusal above is the collision and not "two entries". Each
    // entry keeps the field-name convention its owner is derived from
    // (`items` -> `ItemsEntry`, `others` -> `OthersEntry`), which A22's
    // ownership rule reads.
    {
        const std::string bytes = wire_holder_set(
            wire_nested_map_entry("ItemsEntry", 9, 5) +
                wire_nested_map_entry("OthersEntry", 3, 9),
            wire_message_field(
                2, wire_field("items", 1, 3, 11, ".p.Holder.ItemsEntry")) +
                wire_message_field(
                    2, wire_field("others", 2, 3, 11,
                                  ".p.Holder.OthersEntry")),
            "distinct_map_entries.proto");
        const FileDescriptorSet decoded =
            easypb::decode<FileDescriptorSet>(bytes);
        CHECK(decoded.file.size() == 1);
        if (decoded.file.size() != 1) return;
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& seeded =
            set.add_file(std::string(decoded.file[0].name.data(),
                                     decoded.file[0].name.size()));
        seeded.physical_name.clear();
        seeded.from_descriptor_set = true;
        seeded.file = decoded.file[0];

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }

    // Control 2: the same entry NAME in two different message scopes links in
    // both modes - the collision is per scope, not per file.
    {
        std::string holder = wire_text_field(1, "Holder");
        holder += wire_message_field(
            2, wire_field("items", 1, 3, 11, ".p.Holder.ItemsEntry"));
        holder += wire_nested_map_entry("ItemsEntry", 9, 5);
        std::string other = wire_text_field(1, "Other");
        other += wire_message_field(
            2, wire_field("items", 1, 3, 11, ".p.Other.ItemsEntry"));
        other += wire_nested_map_entry("ItemsEntry", 3, 9);
        std::string file = wire_text_field(1, "scoped_map_entries.proto");
        file += wire_text_field(2, "p");
        file += wire_message_field(4, holder);
        file += wire_message_field(4, other);
        const std::string bytes = wire_message_field(1, file);

        const FileDescriptorSet decoded =
            easypb::decode<FileDescriptorSet>(bytes);
        CHECK(decoded.file.size() == 1);
        if (decoded.file.size() != 1) return;
        CHECK(decoded.file[0].message_type.size() == 2);
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& seeded =
            set.add_file(std::string(decoded.file[0].name.data(),
                                     decoded.file[0].name.size()));
        seeded.physical_name.clear();
        seeded.from_descriptor_set = true;
        seeded.file = decoded.file[0];

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
}

// D-55: a repeated field may not carry a default, which protoc refuses while it
// builds a descriptor and our parser refuses as source text.
void test_descriptor_origin_repeated_default_is_refused()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "rep-default.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "a", 1,
                  FieldDescriptorProto::LABEL_REPEATED, 0);
        FieldDescriptorProto& field = file.file.message_type[0].field[0];
        field.default_value = file.strings.save("5");
        field.has_default_value = true;

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
        CHECK(error.message.find("repeated") != std::string::npos);
    }
    // Controls: the same default on a singular field, and a repeated field with
    // no default, both link.
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "rep-default-ok.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "a", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        FieldDescriptorProto& field = file.file.message_type[0].field[0];
        field.default_value = file.strings.save("5");
        field.has_default_value = true;
        add_field(file, file.file.message_type[0], "b", 2,
                  FieldDescriptorProto::LABEL_REPEATED, 0);

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
}

// D-56: a field claimed by a oneof must be OPTIONAL. The range check keeps its
// own message by running first, and an ABSENT label is read as OPTIONAL because
// that is protoc's own default for the field.
void test_descriptor_origin_oneof_member_label_is_refused()
{
    const int bad_labels[] = {FieldDescriptorProto::LABEL_REPEATED,
                              FieldDescriptorProto::LABEL_REQUIRED};
    for (std::size_t i = 0; i < sizeof(bad_labels) / sizeof(bad_labels[0]);
         ++i) {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "oneof-label.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        DescriptorProto& message = file.file.message_type[0];
        add_field(file, message, "a", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        add_oneof_decl(file, message, "pick");
        claim_oneof(message, 0, 0);
        message.field[0].label = bad_labels[i];

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
        CHECK(error.message.find("oneof") != std::string::npos);
    }
    // Controls: the OPTIONAL member, and the same member with its label absent
    // (protoc's default reads it as optional), both link.
    for (int absent = 0; absent < 2; ++absent) {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "oneof-label-ok.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        DescriptorProto& message = file.file.message_type[0];
        add_field(file, message, "a", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        add_oneof_decl(file, message, "pick");
        claim_oneof(message, 0, 0);
        if (absent == 1) {
            message.field[0].has_label = false;
            message.field[0].label = 0;
        }

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
}

// D-58: a bytes default's escape grammar is checked in the descriptor path, the
// way protoc's `absl::CUnescape` does before the value reaches the generator.
void test_descriptor_origin_bytes_default_escape_is_validated()
{
    const char* bad[] = {"\\q", "abc\\", "\\400", "\\777", "\\x123",
                         "\\8"};
    for (std::size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "bytes-default.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "b", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        FieldDescriptorProto& field = file.file.message_type[0].field[0];
        field.type = FieldDescriptorProto::TYPE_BYTES;
        field.default_value = file.strings.save(bad[i]);
        field.has_default_value = true;

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
        CHECK(error.message.find("escaping") != std::string::npos);
    }
    // Accept controls: the canonical forms protoc itself writes, plus the
    // string twin of a bad escape (a string default is not C-unescaped).
    const char* good[] = {"\\001", "\\377", "\\x41", "plain text", "\\n"};
    for (std::size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "bytes-default-ok.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "b", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        FieldDescriptorProto& field = file.file.message_type[0].field[0];
        field.type = FieldDescriptorProto::TYPE_BYTES;
        field.default_value = file.strings.save(good[i]);
        field.has_default_value = true;

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "string-default.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "M");
        add_field(file, file.file.message_type[0], "s", 1,
                  FieldDescriptorProto::LABEL_OPTIONAL, 0);
        FieldDescriptorProto& field = file.file.message_type[0].field[0];
        field.type = FieldDescriptorProto::TYPE_STRING;
        field.default_value = file.strings.save("\\q");
        field.has_default_value = true;

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
}

// D-57: method names are decoded and checked like every other declaration
// name, and a duplicate method inside one service is a symbol collision.
void test_descriptor_origin_method_names_are_validated()
{
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "method-name.pbs", "pkg");
        file.from_descriptor_set = true;
        ServiceDescriptorProto& service = add_service_named(file, "S");
        add_method(file, service, "bad$name");

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
        CHECK(error.message.find("method") != std::string::npos);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "method-name-empty.pbs", "pkg");
        file.from_descriptor_set = true;
        ServiceDescriptorProto& service = add_service_named(file, "S");
        add_method(file, service, "");

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
    }
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "method-duplicate.pbs", "pkg");
        file.from_descriptor_set = true;
        ServiceDescriptorProto& service = add_service_named(file, "S");
        add_method(file, service, "go");
        add_method(file, service, "go");

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL);
        CHECK(error.message.find("go") != std::string::npos);
    }
    // Control: a well-formed service with two distinct method names whose
    // endpoints resolve to messages of the same file links. The endpoints are
    // required since W-16, so a named-but-bare method is not "well-formed".
    {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "method-ok.pbs", "pkg");
        file.from_descriptor_set = true;
        add_message(file, "Request");
        add_message(file, "Response");
        ServiceDescriptorProto& service = add_service_named(file, "S");
        add_method(file, service, "go", ".pkg.Request", ".pkg.Response");
        add_method(file, service, "stop", ".pkg.Request", ".pkg.Response");

        easypb_schema::Diagnostic error;
        CHECK(accepted_in_both_modes(set, error));
    }
}

// D-59: a malformed package component is reported in the package's own scope
// with the package field's descriptor path, not "in the global scope ... at []".
void test_package_component_diagnostic_names_the_package()
{
    const char* packages[] = {"a..b", "a."};
    for (std::size_t i = 0; i < sizeof(packages) / sizeof(packages[0]); ++i) {
        easypb_schema::SchemaSet set;
        easypb_schema::SchemaFile& file =
            make_simple_file(set, "package-dot.pbs", packages[i]);
        file.from_descriptor_set = true;
        add_message(file, "M");

        easypb_schema::Diagnostic error;
        CHECK(refused_in_both_modes(set, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_SYMBOL_NAME);
        CHECK(error.message.find("package component") != std::string::npos);
        CHECK(error.message.find(packages[i]) != std::string::npos);
        CHECK(error.message.find(" at [2]") != std::string::npos);
        CHECK(error.message.find("the global scope") == std::string::npos);
        CHECK(error.message.find("[]") == std::string::npos);
    }
}

int main(int argc, char** argv)
{
    if (argc < 2 || argv[1][0] == '\0') {
        std::cerr << "usage: test_linker <tests-source-dir>\n"
                  << "The argument is the tests/ source directory: the Step\n"
                  << "7.4 corpus checkpoint links the tracked .pbs fixtures\n"
                  << "under codegen/*/data/. Run via ctest.\n";
        return EXIT_FAILURE;
    }
    const std::string fixture_dir = argv[1];
    {
        // Fail loudly on a wrong argument instead of letting the corpus
        // group read thirty bogus paths.
        std::string probe;
        if (!read_binary_file(
                fixture_dir + "/codegen/maps/data/malformed_map.pbs", probe)) {
            std::cerr << "fixture directory lacks "
                         "codegen/maps/data/malformed_map.pbs: "
                      << fixture_dir << '\n';
            return EXIT_FAILURE;
        }
    }

    test_complete_link_publishes_after_every_stage();
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
    test_conflicting_declaration_reports_one_code();
    test_link_indexes_every_file_once();
    test_resolution_separates_invisible_from_missing();
    test_shadowing_commits_the_matched_aggregate();
    test_non_type_heads_do_not_shadow();
    test_service_and_package_heads_follow_the_two_filters();
    test_invisible_candidates_do_not_shadow();
    test_merged_package_is_visible_when_any_declaring_file_is();
    test_sub_package_does_not_make_the_parent_visible();
    test_enum_value_member_paths();
    test_compound_member_non_type_matches_absolute_spelling();
    test_merged_package_node_is_not_a_type_in_both_spellings();
    test_legal_recursion_stays_legal();
    test_legal_map_fields_resolve_to_their_entry();
    test_foreign_map_entry_references_are_rejected();
    test_map_entry_guard_runs_after_visibility();
    test_map_entry_value_resolves_from_its_own_token();
    test_task2_pair_links_and_its_two_mutations_fail();
    test_descriptor_origin_declared_kind_is_compared_not_rewritten();
    test_descriptor_origin_field_numbers_are_validated();
    test_descriptor_origin_named_kind_needs_type_name();
    test_malformed_map_entry_is_rejected_without_a_reference();
    test_file_syntax_classification();
    test_unsupported_syntax_is_refused_before_resolution();
    test_dangling_oneof_index_is_rejected();
    test_input_order_and_targets_do_not_decide_the_verdict();
    test_already_resolved_input_is_resolved_again();
    test_warning_hygiene_drops_only_stale_unresolved_warnings();
    test_group_field_is_refused();
    test_warning_hygiene_drops_the_paired_default_warning();
    test_seeded_prefix_warning_survives_the_link();
    test_relative_package_head_reference_resolves();
    test_relative_package_head_reference_rejections();
    test_invisible_non_type_answers_one_code_in_both_spellings();
    test_indexed_symbol_kinds_stay_in_the_documented_domain();
    test_package_prefix_needs_a_dot_boundary();
    test_proto3_field_may_not_use_a_proto2_enum();
    test_proto3_map_value_may_not_use_a_proto2_enum();
    test_proto3_field_may_use_a_proto2_message_with_enum_fields();
    test_proto2_fields_may_use_either_enum_syntax();
    test_unsupported_syntax_beats_the_cross_syntax_rule();
    test_cross_syntax_rule_is_silent_in_the_deferred_pass();
    test_compat_accepts_a_classified_external_message();
    test_compat_missing_dependency_without_external_references();
    test_compat_warns_once_per_external_type_name();
    test_compat_rejects_unclassified_external_references();
    test_compat_resolves_local_references_normally();
    test_compat_kind_mismatch_is_the_complete_mode_code();
    test_compat_runs_the_entry_shape_sweep_before_the_finish_pass();
    test_compat_validates_field_structure();
    test_compat_refuses_an_invisible_reference();
    test_compat_preconditions_are_exact();
    test_compat_refuses_editions_like_complete_mode();
    test_compat_binder_still_rejects_malformed_indexes();
    test_compat_tolerates_a_missing_weak_target();
    test_compat_preserves_an_external_enum_default();
    test_compat_then_complete_never_consults_the_escape_hatch();
    test_corpus_compatibility_mode(fixture_dir);
    test_map_entry_sweep_reaches_every_depth();
    test_underscore_map_fields_collide_on_the_shared_entry_name();
    test_descriptor_origin_field_without_a_declared_kind_is_refused();
    test_descriptor_origin_duplicate_enum_number_is_refused();
    test_descriptor_origin_empty_enum_is_refused();
    test_descriptor_origin_proto3_field_rules_are_refused();
    test_descriptor_origin_symbol_names_are_validated();
    test_trailing_dot_reference_is_unresolved_in_both_modes();
    test_descriptor_origin_empty_oneof_is_rejected();
    test_descriptor_origin_editions_field_decides_the_class();
    test_unsupported_syntax_refusal_names_what_decided();
    test_descriptor_origin_packed_false_is_accepted();
    test_malformed_map_entry_shape_is_rejected();
    test_map_entry_extension_data_is_counted_and_refused();
    test_duplicate_map_entry_name_is_a_name_collision();
    test_descriptor_origin_repeated_default_is_refused();
    test_descriptor_origin_oneof_member_label_is_refused();
    test_descriptor_origin_bytes_default_escape_is_validated();
    test_descriptor_origin_method_names_are_validated();
    test_package_component_diagnostic_names_the_package();
    test_pool_growth_after_link_keeps_every_published_name();
    test_set_growth_keeps_file_addresses_and_the_published_view();
    test_descriptor_only_isolation_reports_a_descriptor_path();
    test_repeated_runs_publish_identical_views();
    test_iterator_rules_stability_across_file_order();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all schema linker tests passed\n";
    return EXIT_SUCCESS;
}
