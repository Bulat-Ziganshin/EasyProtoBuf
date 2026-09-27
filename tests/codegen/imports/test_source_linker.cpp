// Source-loader driven tests for the linked-schema layer: real .proto text in
// tests/codegen/imports/data/linker, loaded through load_source_files and then
// linked with link_schema. This is the home of the parser-dependent lifetime
// and source-location tests; the parser-free subset lives in test_linker.cpp.
//
// Registered only inside if(EASYPB_CODEGEN_WITH_PROTO_PARSER) in
// tests/CMakeLists.txt, so descriptor-set-only profiles never compile it.
//
// Fix round r5 (ledger D-15) filled the first case family: the
// relative-reference rows of the code-review lane's corpus, whose verdicts
// protoc 30.1 settles on the SOURCE path
// (build/plan4/fix-r5/oracle/oracle-summary.txt, fixtures inventoried in
// tests/codegen/imports/data/linker/relative/README.md). Before this, no gate
// exercised a relative reference whose first component is a package node: the
// registered .pbs corpus carries absolute names, which is how D-14 survived
// six oracle-measured rounds. The remaining Step-8.2 families (source
// locations, oracle decoding, buffer lifetime) are still owned by Step 8.

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "proto_loader.hpp"
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

struct RelativeCase {
    const char* root;
    bool accept;
    const char* substring;
};

// Loads one root with the fixture directory as its only search root, links in
// complete mode, and checks the verdict against protoc's. Every rejection in
// this family is a not-defined rejection naming the reference - protoc words
// them `"<name>" is not defined` - and a rejection publishes nothing (E3/A1).
void run_relative_case(const std::string& directory, const RelativeCase& item)
{
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(directory);
    const std::vector<std::string> roots(1, std::string(item.root));
    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, set, error));

    easypb_schema::LinkedSchema linked;
    const bool accepted = easypb_schema::link_schema(
        set, easypb_schema::LINK_COMPLETE, linked, error);
    CHECK(accepted == item.accept);
    if (item.accept) {
        CHECK(linked.files == &set);
        CHECK(linked.complete);
        return;
    }
    CHECK(error.code == easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE);
    CHECK(error.message.find(item.substring) != std::string::npos);
    CHECK(linked.files == 0);
    CHECK(!linked.complete);
}

// A reference whose first component is a package node, spelled relatively, is
// resolved from the source path exactly as protoc resolves it: the parent
// package of a visible file's package covers the head when the member exists
// (r01 no package, r03 the same package spelled out, r06 through a public
// import), and the shapes protoc rejects - a partially visible deeper name
// (r05) and an import-free reference (r08) - are rejected as not defined.
void test_relative_references_match_protoc(const std::string& directory)
{
    const RelativeCase accepts[] = {
        {"r01_nopkg_to_subpkg.proto", true, 0},
        {"r03_same_pkg.proto", true, 0},
        {"r06_pub_import.proto", true, 0},
    };
    for (std::size_t i = 0; i < sizeof(accepts) / sizeof(accepts[0]); ++i) {
        run_relative_case(directory, accepts[i]);
    }

    const RelativeCase rejects[] = {
        {"r05_partial_visible.proto", false, "shared.deep.D"},
        {"r08_no_import.proto", false, "a.b.Type"},
    };
    for (std::size_t i = 0; i < sizeof(rejects) / sizeof(rejects[0]); ++i) {
        run_relative_case(directory, rejects[i]);
    }
}

// Fix round r10 (ledger D-21): the head filter is TWO filters, and this pair
// measures both directions on the source path. A SERVICE may commit the head
// of a COMPOUND reference (protoc's `Symbol::IsAggregate()`), and the
// committed head then fails the member lookup instead of falling outward to
// the imported global `Q.R` (h01, rejected by protoc 30.1 as not defined);
// the same service may never win a WHOLE-name reference (`Symbol::IsType()`),
// so the walk continues outward and the imported outer message `Q` links
// (h02, accepted with the descriptor-set validator's re-read at exit 0).
// Both verdicts, measured against these bytes:
// build/plan4/fix-r10/corpus/oracle-corpus-summary.txt; the wider case group
// (package and field halves) is build/plan4/fix-r10/oracle/oracle-summary.txt.
//
// The pair sits at the fixture ROOT rather than in `relative/`, because the
// pack's Step 8.3 command - and so the differential lane - uses the root as
// `--proto_path`: a family whose imports are spelled for a nested proto root
// cannot be generated by protoc there, and a not-found artifact is not
// distinguishable from a rejection in the lane's log.
void test_service_head_cases_match_protoc(const std::string& directory)
{
    const RelativeCase accepts[] = {
        {"h02_service_head_whole_name.proto", true, 0},
    };
    for (std::size_t i = 0; i < sizeof(accepts) / sizeof(accepts[0]); ++i) {
        run_relative_case(directory, accepts[i]);
    }

    const RelativeCase rejects[] = {
        {"h01_service_head_compound.proto", false, "unresolved type Q.R in "},
    };
    for (std::size_t i = 0; i < sizeof(rejects) / sizeof(rejects[0]); ++i) {
        run_relative_case(directory, rejects[i]);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: codegen_imports_source_linker_test <fixture-dir>\n";
        return EXIT_FAILURE;
    }
    const std::string data_dir(argv[1]);
    test_relative_references_match_protoc(data_dir + "/relative");
    test_service_head_cases_match_protoc(data_dir);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all source linker tests passed\n";
    return EXIT_SUCCESS;
}
