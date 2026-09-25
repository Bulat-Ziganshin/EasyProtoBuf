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

// Step 2.2's temporary contract. It replaced Step 1's "not implemented"
// expectation and Step 5.1 replaces its success half with real linking.
void test_link_stub_publishes_stages_2_and_4()
{
    // Success path: stages 2 and 4 ran, the view is published, and it is
    // explicitly incomplete -- stage 3 does not exist yet and stage 5 is not
    // implemented.
    easypb_schema::SchemaSet set;
    const Chain chain = make_chain_fixture(set, false, false);
    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;

    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                     linked, error));
    CHECK(linked.files == &set);
    CHECK(!linked.complete);
    CHECK(linked.symbols.symbols.empty());     // stage 3 is not wired yet
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

} // namespace

int main()
{
    test_link_stub_publishes_stages_2_and_4();
    test_visibility_ordinary_import_is_not_transitive();
    test_visibility_public_import_is_transitive();
    test_visibility_root_status_grants_nothing();
    test_visibility_same_package_without_import_is_invisible();
    test_visibility_weak_import_behaves_as_ordinary();
    test_visibility_diamond_public_exports_are_complete();
    test_visibility_public_chain_re_exports_recursively();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all schema linker tests passed\n";
    return EXIT_SUCCESS;
}
