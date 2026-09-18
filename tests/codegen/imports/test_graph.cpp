// Tests for the common import graph (codegen/schema.hpp bind_import_edges
// plus codegen/logical_paths.hpp). This test links only easypb_schema and
// must keep passing in descriptor-set-only builds. All fixtures are
// in-memory descriptors with empty physical_name and unknown positions;
// graph failures never consult the filesystem and never require parser
// metadata.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "logical_paths.hpp"
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

easypb_schema::SourceLocation at_line(int line)
{
    easypb_schema::SourceLocation location;
    location.offset = 0;
    location.line = static_cast<std::size_t>(line);
    location.column = 1;
    return location;
}

// Adds a descriptor-origin file with the given logical name and
// dependency lists. Strings are interned in the file's own pool.
// physical_name is left empty to prove the binder needs no filesystem
// identity; imports are left empty unless the caller adds coordinates.
easypb_schema::SchemaFile& make_file(easypb_schema::SchemaSet& set,
                                     const std::string& logical,
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

std::size_t count_occurrences(const std::string& haystack, const std::string& needle)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = haystack.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

void test_logical_paths_valid()
{
    CHECK(easypb_schema::is_valid_logical_path("a.proto"));
    CHECK(easypb_schema::is_valid_logical_path("a/b.proto"));
    CHECK(easypb_schema::is_valid_logical_path("common/state.proto"));
    CHECK(easypb_schema::is_valid_logical_path("a-b_c.proto"));
    CHECK(easypb_schema::is_valid_logical_path("a/b-c_d.proto"));
    CHECK(easypb_schema::is_valid_logical_path("x"));
}

void test_logical_paths_invalid()
{
    const char* invalid[] = {
        "",
        "/a.proto",
        "C:/a.proto",
        "C:a.proto",
        "a:b.proto",
        "a\\b.proto",
        "a//b.proto",
        "a/b/",
        "/a",
        "a/./b.proto",
        "a/../b.proto",
        ".",
        "..",
        "a/\"b.proto",
        "a/'b.proto",
        "a/.",
        "a/.."
    };
    for (std::size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        std::string reason;
        CHECK(!easypb_schema::is_valid_logical_path(invalid[i]));
        CHECK(!easypb_schema::validate_logical_path(invalid[i], reason));
        CHECK(!reason.empty());
    }
    {
        std::string with_control = std::string("a/b") + char(1) + ".proto";
        CHECK(!easypb_schema::is_valid_logical_path(with_control));
    }
    {
        std::string with_del = std::string("a/b") + char(0x7f) + ".proto";
        CHECK(!easypb_schema::is_valid_logical_path(with_del));
    }
}

void test_diagnostic_codes_stay_distinct()
{
    CHECK(easypb_schema::DIAGNOSTIC_GENERIC !=
          easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
    CHECK(easypb_schema::DIAGNOSTIC_INVALID_IMPORT !=
          easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(easypb_schema::DIAGNOSTIC_MISSING_IMPORT !=
          easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(easypb_schema::DIAGNOSTIC_IMPORT_CYCLE !=
          easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(easypb_schema::DIAGNOSTIC_INVALID_IMPORT !=
          easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
    CHECK(easypb_schema::DIAGNOSTIC_INVALID_IMPORT !=
          easypb_schema::DIAGNOSTIC_IMPORT_CYCLE);
    CHECK(easypb_schema::DIAGNOSTIC_MISSING_IMPORT !=
          easypb_schema::DIAGNOSTIC_IMPORT_CYCLE);
}

void test_bind_simple_chain()
{
    easypb_schema::SchemaSet files;
    make_file(files, "c.proto", std::vector<std::string>(),
              std::vector<std::int32_t>(), std::vector<std::int32_t>());
    {
        std::vector<std::string> deps;
        deps.push_back("c.proto");
        make_file(files, "b.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
    }
    {
        std::vector<std::string> deps;
        deps.push_back("b.proto");
        make_file(files, "a.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
    }
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::bind_import_edges(files, false, error));
    easypb_schema::SchemaFile* a = files.find_file("a.proto");
    easypb_schema::SchemaFile* b = files.find_file("b.proto");
    easypb_schema::SchemaFile* c = files.find_file("c.proto");
    CHECK(a != 0 && b != 0 && c != 0);
    if (!a || !b || !c) return;
    CHECK(a->edges.size() == 1);
    CHECK(b->edges.size() == 1);
    CHECK(c->edges.empty());
    if (a->edges.size() == 1) {
        CHECK(a->edges[0].target == b);
        CHECK(a->edges[0].dependency_index == 0);
        CHECK(a->edges[0].modifier ==
              easypb_schema::ImportInfo::NORMAL_IMPORT);
    }
    if (b->edges.size() == 1) CHECK(b->edges[0].target == c);
}

void test_bind_diamond_preserves_order()
{
    easypb_schema::SchemaSet files;
    make_file(files, "shared.proto", std::vector<std::string>(),
              std::vector<std::int32_t>(), std::vector<std::int32_t>());
    {
        std::vector<std::string> deps;
        deps.push_back("shared.proto");
        make_file(files, "left.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
        make_file(files, "right.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
    }
    {
        std::vector<std::string> deps;
        deps.push_back("left.proto");
        deps.push_back("right.proto");
        make_file(files, "root.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
    }
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::bind_import_edges(files, false, error));
    easypb_schema::SchemaFile* root = files.find_file("root.proto");
    CHECK(root != 0);
    if (!root || root->edges.size() != 2) {
        CHECK(root && root->edges.size() == 2);
        return;
    }
    CHECK(root->edges[0].target == files.find_file("left.proto"));
    CHECK(root->edges[1].target == files.find_file("right.proto"));
    CHECK(root->edges[0].dependency_index == 0);
    CHECK(root->edges[1].dependency_index == 1);
}

void test_bind_modifiers()
{
    easypb_schema::SchemaSet files;
    make_file(files, "a.proto", std::vector<std::string>(),
              std::vector<std::int32_t>(), std::vector<std::int32_t>());
    make_file(files, "b.proto", std::vector<std::string>(),
              std::vector<std::int32_t>(), std::vector<std::int32_t>());
    make_file(files, "c.proto", std::vector<std::string>(),
              std::vector<std::int32_t>(), std::vector<std::int32_t>());
    {
        std::vector<std::string> deps;
        deps.push_back("a.proto");
        deps.push_back("b.proto");
        deps.push_back("c.proto");
        std::vector<std::int32_t> public_indexes;
        public_indexes.push_back(1);
        std::vector<std::int32_t> weak_indexes;
        weak_indexes.push_back(2);
        make_file(files, "root.proto", deps, public_indexes, weak_indexes);
    }
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::bind_import_edges(files, false, error));
    easypb_schema::SchemaFile* root = files.find_file("root.proto");
    CHECK(root != 0);
    if (!root || root->edges.size() != 3) return;
    CHECK(root->edges[0].modifier == easypb_schema::ImportInfo::NORMAL_IMPORT);
    CHECK(root->edges[1].modifier == easypb_schema::ImportInfo::PUBLIC_IMPORT);
    CHECK(root->edges[2].modifier == easypb_schema::ImportInfo::WEAK_IMPORT);
}

void test_bind_rejects_bad_public_index()
{
    // Descriptor public index equals dependency count.
    easypb_schema::SchemaSet files;
    make_file(files, "a.proto", std::vector<std::string>(),
              std::vector<std::int32_t>(), std::vector<std::int32_t>());
    {
        std::vector<std::string> deps;
        deps.push_back("a.proto");
        std::vector<std::int32_t> public_indexes;
        public_indexes.push_back(1);
        make_file(files, "root.proto", deps, public_indexes,
                  std::vector<std::int32_t>());
    }
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::bind_import_edges(files, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.file == "root.proto");
    CHECK(error.message.find("public dependency index") != std::string::npos);
}

void test_bind_rejects_negative_weak_index()
{
    easypb_schema::SchemaSet files;
    {
        std::vector<std::string> deps;
        deps.push_back("a.proto");
        std::vector<std::int32_t> weak_indexes;
        weak_indexes.push_back(-1);
        make_file(files, "root.proto", deps, std::vector<std::int32_t>(),
                  weak_indexes);
    }
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::bind_import_edges(files, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("weak dependency index") != std::string::npos);
}

void test_bind_rejects_duplicate_and_overlapping_indexes()
{
    {
        easypb_schema::SchemaSet files;
        make_file(files, "a.proto", std::vector<std::string>(),
                  std::vector<std::int32_t>(), std::vector<std::int32_t>());
        make_file(files, "b.proto", std::vector<std::string>(),
                  std::vector<std::int32_t>(), std::vector<std::int32_t>());
        std::vector<std::string> deps;
        deps.push_back("a.proto");
        deps.push_back("b.proto");
        std::vector<std::int32_t> public_indexes;
        public_indexes.push_back(0);
        public_indexes.push_back(0);
        easypb_schema::SchemaFile& root =
            make_file(files, "root.proto", deps, public_indexes,
                      std::vector<std::int32_t>());
        // The duplicated index is valid, so its declaration site must
        // survive in the diagnostic.
        easypb_schema::ImportInfo first;
        first.path = "a.proto";
        first.modifier = easypb_schema::ImportInfo::PUBLIC_IMPORT;
        first.location = at_line(2);
        root.imports.push_back(first);
        easypb_schema::ImportInfo second;
        second.path = "b.proto";
        second.modifier = easypb_schema::ImportInfo::NORMAL_IMPORT;
        second.location = at_line(3);
        root.imports.push_back(second);
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::bind_import_edges(files, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
        CHECK(error.message.find("duplicate public") != std::string::npos);
        CHECK(error.location.line == 2);
    }
    {
        easypb_schema::SchemaSet files;
        make_file(files, "a.proto", std::vector<std::string>(),
                  std::vector<std::int32_t>(), std::vector<std::int32_t>());
        make_file(files, "b.proto", std::vector<std::string>(),
                  std::vector<std::int32_t>(), std::vector<std::int32_t>());
        std::vector<std::string> deps;
        deps.push_back("a.proto");
        deps.push_back("b.proto");
        std::vector<std::int32_t> weak_indexes;
        weak_indexes.push_back(1);
        weak_indexes.push_back(1);
        easypb_schema::SchemaFile& root =
            make_file(files, "root.proto", deps, std::vector<std::int32_t>(),
                      weak_indexes);
        easypb_schema::ImportInfo first;
        first.path = "a.proto";
        first.modifier = easypb_schema::ImportInfo::NORMAL_IMPORT;
        first.location = at_line(2);
        root.imports.push_back(first);
        easypb_schema::ImportInfo second;
        second.path = "b.proto";
        second.modifier = easypb_schema::ImportInfo::WEAK_IMPORT;
        second.location = at_line(4);
        root.imports.push_back(second);
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::bind_import_edges(files, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
        CHECK(error.message.find("duplicate weak") != std::string::npos);
        CHECK(error.location.line == 4);
    }
    {
        easypb_schema::SchemaSet files;
        make_file(files, "a.proto", std::vector<std::string>(),
                  std::vector<std::int32_t>(), std::vector<std::int32_t>());
        std::vector<std::string> deps;
        deps.push_back("a.proto");
        std::vector<std::int32_t> public_indexes;
        public_indexes.push_back(0);
        std::vector<std::int32_t> weak_indexes;
        weak_indexes.push_back(0);
        make_file(files, "root.proto", deps, public_indexes, weak_indexes);
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::bind_import_edges(files, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
        CHECK(error.message.find("both public and weak") != std::string::npos);
    }
}

void test_bind_rejects_repeated_dependency_names()
{
    easypb_schema::SchemaSet files;
    make_file(files, "a.proto", std::vector<std::string>(),
              std::vector<std::int32_t>(), std::vector<std::int32_t>());
    {
        std::vector<std::string> deps;
        deps.push_back("a.proto");
        deps.push_back("a.proto");
        make_file(files, "root.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
    }
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::bind_import_edges(files, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("duplicate import") != std::string::npos);
    CHECK(error.message.find("a.proto") != std::string::npos);
}

void test_bind_rejects_invalid_spelling()
{
    easypb_schema::SchemaSet files;
    {
        std::vector<std::string> deps;
        deps.push_back("../outside.proto");
        make_file(files, "root.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
    }
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::bind_import_edges(files, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.file == "root.proto");
    CHECK(error.message.find("outside.proto") != std::string::npos);
}

void test_bind_missing_normal_and_weak()
{
    {
        easypb_schema::SchemaSet files;
        std::vector<std::string> deps;
        deps.push_back("missing.proto");
        make_file(files, "root.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::bind_import_edges(files, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
        CHECK(error.file == "root.proto");
        CHECK(error.message.find("missing.proto") != std::string::npos);
        // A missing unused import is still an error in complete mode.
        CHECK(error.message.find("missing") != std::string::npos);
    }
    {
        easypb_schema::SchemaSet files;
        std::vector<std::string> deps;
        deps.push_back("missing-weak.proto");
        std::vector<std::int32_t> weak_indexes;
        weak_indexes.push_back(0);
        make_file(files, "root.proto", deps, std::vector<std::int32_t>(),
                  weak_indexes);
        // Weak has the same existence requirement in complete mode and
        // the diagnostic identifies the weak declaration.
        easypb_schema::SchemaFile* root = files.find_file("root.proto");
        CHECK(root != 0);
        if (root != 0) {
            easypb_schema::ImportInfo info;
            info.path = "missing-weak.proto";
            info.modifier = easypb_schema::ImportInfo::WEAK_IMPORT;
            info.location = easypb_schema::unknown_location();
            root->imports.push_back(info);
        }
        easypb_schema::Diagnostic error;
        CHECK(!easypb_schema::bind_import_edges(files, false, error));
        CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
        CHECK(error.message.find("weak") != std::string::npos);
        CHECK(error.message.find("missing-weak.proto") != std::string::npos);
    }
}

void test_bind_allow_missing_keeps_null_edge()
{
    easypb_schema::SchemaSet files;
    {
        std::vector<std::string> deps;
        deps.push_back("present.proto");
        deps.push_back("absent.proto");
        std::vector<std::int32_t> weak_indexes;
        weak_indexes.push_back(1);
        make_file(files, "root.proto", deps, std::vector<std::int32_t>(),
                  weak_indexes);
    }
    make_file(files, "present.proto", std::vector<std::string>(),
              std::vector<std::int32_t>(), std::vector<std::int32_t>());
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::bind_import_edges(files, true, error));
    easypb_schema::SchemaFile* root = files.find_file("root.proto");
    CHECK(root != 0);
    if (!root || root->edges.size() != 2) return;
    CHECK(root->edges[0].target == files.find_file("present.proto"));
    CHECK(root->edges[1].target == 0);
    CHECK(root->edges[1].dependency_index == 1);
    CHECK(root->edges[1].modifier == easypb_schema::ImportInfo::WEAK_IMPORT);
    // The same thin input is rejected under complete linking.
    CHECK(!easypb_schema::bind_import_edges(files, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
}

void test_bind_self_cycle_names_file_twice()
{
    easypb_schema::SchemaSet files;
    {
        std::vector<std::string> deps;
        deps.push_back("a.proto");
        easypb_schema::SchemaFile& a = make_file(files, "a.proto", deps,
                                                 std::vector<std::int32_t>(),
                                                 std::vector<std::int32_t>());
        easypb_schema::ImportInfo info;
        info.path = "a.proto";
        info.modifier = easypb_schema::ImportInfo::NORMAL_IMPORT;
        info.location = at_line(3);
        a.imports.push_back(info);
    }
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::bind_import_edges(files, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_IMPORT_CYCLE);
    CHECK(count_occurrences(error.message, "a.proto") >= 2);
    CHECK(error.message.find("closes the cycle") != std::string::npos);
    CHECK(error.location.line == 3);
}

void test_bind_two_node_cycle_names_both_edges()
{
    easypb_schema::SchemaSet files;
    {
        std::vector<std::string> deps;
        deps.push_back("b.proto");
        easypb_schema::SchemaFile& a = make_file(files, "a.proto", deps,
                                                 std::vector<std::int32_t>(),
                                                 std::vector<std::int32_t>());
        easypb_schema::ImportInfo info;
        info.path = "b.proto";
        info.modifier = easypb_schema::ImportInfo::NORMAL_IMPORT;
        info.location = at_line(5);
        a.imports.push_back(info);
    }
    {
        std::vector<std::string> deps;
        deps.push_back("a.proto");
        easypb_schema::SchemaFile& b = make_file(files, "b.proto", deps,
                                                 std::vector<std::int32_t>(),
                                                 std::vector<std::int32_t>());
        easypb_schema::ImportInfo info;
        info.path = "a.proto";
        info.modifier = easypb_schema::ImportInfo::NORMAL_IMPORT;
        info.location = at_line(7);
        b.imports.push_back(info);
    }
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::bind_import_edges(files, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_IMPORT_CYCLE);
    CHECK(error.message.find("a.proto") != std::string::npos);
    CHECK(error.message.find("b.proto") != std::string::npos);
    CHECK(error.message.find("closes the cycle") != std::string::npos);
}

void test_bind_nested_cycle_reports_root_chain()
{
    // Insertion order defines the DFS roots with root.proto first, so
    // the diagnostic must carry the whole root-to-failure path rather
    // than only the closed a -> b -> a loop.
    easypb_schema::SchemaSet files;
    {
        std::vector<std::string> deps;
        deps.push_back("middle.proto");
        make_file(files, "root.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
    }
    {
        std::vector<std::string> deps;
        deps.push_back("a.proto");
        make_file(files, "middle.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
    }
    {
        std::vector<std::string> deps;
        deps.push_back("b.proto");
        make_file(files, "a.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
    }
    {
        std::vector<std::string> deps;
        deps.push_back("a.proto");
        make_file(files, "b.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
    }
    easypb_schema::Diagnostic error;
    CHECK(!easypb_schema::bind_import_edges(files, false, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_IMPORT_CYCLE);
    CHECK(error.message.find(
        "root.proto -> middle.proto -> a.proto -> b.proto -> a.proto") !=
        std::string::npos);
}

void test_bind_uses_descriptors_not_import_positions()
{
    // The descriptor list is authoritative: an imports entry naming a
    // different file must not change edge meaning.
    easypb_schema::SchemaSet files;
    make_file(files, "real.proto", std::vector<std::string>(),
              std::vector<std::int32_t>(), std::vector<std::int32_t>());
    {
        std::vector<std::string> deps;
        deps.push_back("real.proto");
        easypb_schema::SchemaFile& importer =
            make_file(files, "root.proto", deps, std::vector<std::int32_t>(),
                      std::vector<std::int32_t>());
        easypb_schema::ImportInfo misleading;
        misleading.path = "fake.proto";
        misleading.modifier = easypb_schema::ImportInfo::NORMAL_IMPORT;
        misleading.location = at_line(2);
        importer.imports.push_back(misleading);
    }
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::bind_import_edges(files, false, error));
    easypb_schema::SchemaFile* root = files.find_file("root.proto");
    CHECK(root != 0);
    if (root && !root->edges.empty()) {
        CHECK(root->edges[0].target == files.find_file("real.proto"));
    }
}

void test_bind_rebuilds_edges()
{
    easypb_schema::SchemaSet files;
    make_file(files, "b.proto", std::vector<std::string>(),
              std::vector<std::int32_t>(), std::vector<std::int32_t>());
    {
        std::vector<std::string> deps;
        deps.push_back("b.proto");
        make_file(files, "a.proto", deps, std::vector<std::int32_t>(),
                  std::vector<std::int32_t>());
    }
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::bind_import_edges(files, false, error));
    CHECK(easypb_schema::bind_import_edges(files, false, error));
    easypb_schema::SchemaFile* a = files.find_file("a.proto");
    CHECK(a != 0);
    if (a != 0) CHECK(a->edges.size() == 1);
}

void test_bind_empty_set_succeeds()
{
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::bind_import_edges(files, false, error));
    CHECK(easypb_schema::bind_import_edges(files, true, error));
}

} // namespace

int main()
{
    test_logical_paths_valid();
    test_logical_paths_invalid();
    test_diagnostic_codes_stay_distinct();
    test_bind_simple_chain();
    test_bind_diamond_preserves_order();
    test_bind_modifiers();
    test_bind_rejects_bad_public_index();
    test_bind_rejects_negative_weak_index();
    test_bind_rejects_duplicate_and_overlapping_indexes();
    test_bind_rejects_repeated_dependency_names();
    test_bind_rejects_invalid_spelling();
    test_bind_missing_normal_and_weak();
    test_bind_allow_missing_keeps_null_edge();
    test_bind_self_cycle_names_file_twice();
    test_bind_two_node_cycle_names_both_edges();
    test_bind_nested_cycle_reports_root_chain();
    test_bind_uses_descriptors_not_import_positions();
    test_bind_rebuilds_edges();
    test_bind_empty_set_succeeds();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all import graph tests passed\n";
    return EXIT_SUCCESS;
}
