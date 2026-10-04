// Source-loader driven tests for the linked-schema layer: real .proto text in
// tests/codegen/imports/data/linker, loaded through load_source_files and then
// linked with link_schema. This is the home of the parser-dependent lifetime,
// source-location and oracle-comparison tests; the parser-free subset lives in
// test_linker.cpp.
//
// Registered only inside if(EASYPB_CODEGEN_WITH_PROTO_PARSER) in
// tests/CMakeLists.txt, so descriptor-set-only profiles never compile it.
// tests/CMakeLists.txt passes two directories: the fixture corpus and the
// oracle directory build/package-imports/oracle Step 8.3's lane generates.
//
// Fix round r5 (ledger D-15) filled the first case family: the
// relative-reference rows of the code-review lane's corpus, whose verdicts
// protoc 30.1 settles on the SOURCE path
// (build/plan4/fix-r5/oracle/oracle-summary.txt, fixtures inventoried in
// tests/codegen/imports/data/linker/relative/README.md). Fix round r10
// (ledger D-21) added the service-head pair at the corpus root. Step 8 adds
// the real source locations of a refusal and the protoc differential (8.3):
// for every accepting fixture the generated descriptor set is decoded with the
// repository's own decoder and each linked field's absolute type_name and kind
// is compared against it, exactly - no suffix-name equivalence
// (Step 8.2/8.3 of plan4/steps/05-exit-validation.md).

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
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

std::string text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

bool read_binary_file(const std::string& path, std::string& contents)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    contents.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
    return true;
}

// ---------------------------------------------------------------------------
// Verdict families (fix rounds r5 and r10)
// ---------------------------------------------------------------------------

struct RelativeCase {
    const char* root;
    bool accept;
    const char* substring;
};

// Loads one root with the given directory as its only search root, links in
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
// differential lane gives every fixture the directory that owns it as
// `--proto_path` (Step 8.3, ledger D-22): a family whose imports are spelled
// for a nested proto root cannot be generated from the corpus root, and a
// not-found artifact is not distinguishable from a rejection in the lane's log.
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

// ---------------------------------------------------------------------------
// 8.2: a source refusal carries a real location
// ---------------------------------------------------------------------------

// The pack is explicit that "not (0,0)" is not an assertion: the named line and
// column are the type token's. `relative/r08_no_import.proto` is one line,
// `message M { optional a.b.Type t = 1; }`, and `a.b.Type` is the reference the
// failure walk stops at - so the numbers below are that token's, and a parser
// that reported `unknown_location()` or the statement's start would fail here.
void test_source_locations_point_at_the_type_token(
    const std::string& directory)
{
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(directory);
    const std::vector<std::string> roots(1, std::string("r08_no_import.proto"));
    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, set, error));

    easypb_schema::LinkedSchema linked;
    CHECK(!easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                      linked, error));
    CHECK(error.file == "r08_no_import.proto");
    CHECK(error.location.line == 1);
    CHECK(error.location.column == 22);      // the `a` of `a.b.Type`
    std::cout << "LOCATION: " << error.file << ':' << error.location.line << ':'
              << error.location.column << '\n';
}

// ---------------------------------------------------------------------------
// 8.3: the protoc differential
// ---------------------------------------------------------------------------

// One fixture of the registered corpus. `accepts` is protoc 30.1's verdict,
// measured by Step 8.3's protocol and recorded in the inventory README
// (`tests/codegen/imports/data/linker/README.md`): the fixture is compiled with
// the directory that OWNS it as `--proto_path`, one invocation, never
// `--include_imports`, and the artifact's existence is that verdict re-derived
// at run time. `extra_root` is a second file loaded WITH the fixture - a
// fixture named on the command line but never imported, which is the only way
// Step 8.2's "a second root grants nothing" shapes (and Appendix C B6/B10's
// nearer invisible head) can exist in a compilation at all.
struct DifferentialCase {
    const char* fixture;
    bool accepts;
    const char* extra_root;
};

const DifferentialCase kDifferentialCases[] = {
    // The corpus root: the plan-4 regression pair and the head-filter pair.
    {"dep_outer_q.proto", true, 0},
    {"h01_service_head_compound.proto", false, 0},
    {"h02_service_head_whole_name.proto", true, 0},
    {"report.proto", true, 0},
    {"state.proto", true, 0},
    // chain/ - the ordinary edge hides the leaf, the public edge re-exports it.
    {"chain/ordinary/a.proto", false, 0},
    {"chain/ordinary/b.proto", true, 0},
    {"chain/ordinary/c.proto", true, 0},
    {"chain/public/a.proto", true, 0},
    {"chain/public/b.proto", true, 0},
    {"chain/public/c.proto", true, 0},
    // shadowing/ - the Choice.Item fixture and its three counter-fixtures.
    {"shadowing/dep.proto", true, 0},
    {"shadowing/nearer_message.proto", false, 0},
    {"shadowing/nearer_field.proto", true, 0},
    {"shadowing/nearer_enum_value.proto", true, 0},
    {"shadowing/hidden.proto", true, 0},
    {"shadowing/vis.proto", true, 0},
    {"shadowing/nearer_invisible.proto", true, "hidden.proto"},
    // cross_syntax/ - C2 rejects, C3 accepts, C4 (zero-first enum) rejects.
    {"cross_syntax/c2/base2.proto", true, 0},
    {"cross_syntax/c2/root.proto", false, 0},
    {"cross_syntax/c3/msg2.proto", true, 0},
    {"cross_syntax/c3/root.proto", true, 0},
    {"cross_syntax/c4/base2.proto", true, 0},
    {"cross_syntax/c4/root.proto", false, 0},
    // map_entry/ - the control accepts, every user reference to an entry rejects.
    {"map_entry/control.proto", true, 0},
    {"map_entry/dep.proto", true, 0},
    {"map_entry/own_simple.proto", false, 0},
    {"map_entry/own_compound.proto", false, 0},
    {"map_entry/other_entry.proto", false, 0},
    {"map_entry/imported_absolute.proto", false, 0},
    {"map_entry/imported_relative.proto", false, 0},
    {"map_entry/repeated_simple.proto", false, 0},
    {"map_entry/repeated_absolute.proto", false, 0},
    // nested/ - an imported nested message and an imported nested enum.
    {"nested/dep.proto", true, 0},
    {"nested/root.proto", true, 0},
    // service_as_type/ - a retained service name is not a type.
    {"service_as_type/root.proto", false, 0},
    // visibility_roots/ - R3's two cases, each with the declarer as a second root.
    {"visibility_roots/other.proto", true, 0},
    {"visibility_roots/same_package.proto", false, "other.proto"},
    {"visibility_roots/second_root.proto", false, "other.proto"},
    // defaults/ - the accepting control and R13's three rejecting shapes.
    {"defaults/legal.proto", true, 0},
    {"defaults/enum_default_absent.proto", false, 0},
    {"defaults/message_default.proto", false, 0},
    {"defaults/packed_not_packable.proto", false, 0},
    // relative/ - fix round r5's source-path reference family.
    {"relative/dep_ab.proto", true, 0},
    {"relative/dep_pub.proto", true, 0},
    {"relative/dep_shared.proto", true, 0},
    {"relative/r01_nopkg_to_subpkg.proto", true, 0},
    {"relative/r03_same_pkg.proto", true, 0},
    {"relative/r05_partial_visible.proto", false, 0},
    {"relative/r06_pub_import.proto", true, 0},
    {"relative/r08_no_import.proto", false, 0},
};

// Recursively compares a linked message tree against the oracle's. Every field
// the reference compiler resolved to a named type must agree on the absolute
// type_name AND the kind, position for position; scalars carry no type_name on
// either side and their kind is protoc's own, so they are not the comparison.
// The message and field counts are compared too, so a fixture that links with a
// dropped or invented field cannot pass on a prefix of the walk.
void compare_messages(const std::vector<DescriptorProto>& ours,
                      const std::vector<DescriptorProto>& oracle,
                      const std::string& where)
{
    CHECK(ours.size() == oracle.size());
    const std::size_t messages =
        ours.size() < oracle.size() ? ours.size() : oracle.size();
    for (std::size_t i = 0; i < messages; ++i) {
        const std::string scope = where + "." + text(oracle[i].name);
        CHECK(ours[i].field.size() == oracle[i].field.size());
        const std::size_t fields =
            ours[i].field.size() < oracle[i].field.size()
                ? ours[i].field.size() : oracle[i].field.size();
        for (std::size_t f = 0; f < fields; ++f) {
            const FieldDescriptorProto& mine = ours[i].field[f];
            const FieldDescriptorProto& theirs = oracle[i].field[f];
            if (!theirs.has_type_name) continue;
            CHECK(text(mine.type_name) == text(theirs.type_name));
            CHECK(mine.type == theirs.type);
            if (text(mine.type_name) != text(theirs.type_name)
                || mine.type != theirs.type) {
                std::cerr << scope << '.' << text(theirs.name)
                          << ": ours " << text(mine.type_name) << " kind "
                          << static_cast<int>(mine.type)
                          << " vs protoc " << text(theirs.type_name) << " kind "
                          << static_cast<int>(theirs.type) << '\n';
            }
        }
        compare_messages(ours[i].nested_type, oracle[i].nested_type, scope);
    }
}

// The lane's artifact name for a fixture: the corpus-relative path with `/`
// replaced by `_` and `.proto` by `.pb` (matrix.py's own mangling, so the
// suite reads exactly what the lane wrote).
std::string artifact_name(const std::string& fixture)
{
    std::string stem = fixture;
    for (std::size_t i = 0; i < stem.size(); ++i) {
        if (stem[i] == '/') stem[i] = '_';
    }
    CHECK(stem.size() > 6 && stem.substr(stem.size() - 6) == ".proto");
    return stem.substr(0, stem.size() - 6) + ".pb";
}

// Loads one fixture the way the lane generated it - its OWNING directory is the
// only search root and the fixture is named by its bare name - then links it in
// complete mode and compares the result with the artifact. An accepting fixture
// whose artifact is absent prints the pack's `SKIP:` marker and is counted, so
// a clean checkout is honest rather than silent (Step 8.3); a fixture the table
// records as rejected must be rejected here, with no artifact involved.
void run_differential_case(const std::string& corpus,
                           const std::string& oracle_dir,
                           const DifferentialCase& item,
                           std::size_t& compared, std::size_t& skipped)
{
    const std::string fixture = item.fixture;
    const std::string::size_type slash = fixture.rfind('/');
    const std::string owner =
        slash == std::string::npos ? std::string() : fixture.substr(0, slash);
    const std::string root =
        slash == std::string::npos ? fixture : fixture.substr(slash + 1);
    const std::string directory =
        owner.empty() ? corpus : corpus + "/" + owner;

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(directory);
    std::vector<std::string> roots(1, root);
    // The second root is named on the command line and never imported: it puts
    // the declaration in the index without granting visibility.
    if (item.extra_root != 0) roots.push_back(std::string(item.extra_root));
    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    const bool loaded =
        easypb_proto::load_source_files(options, roots, set, error);
    if (!loaded) {
        // The frontend owns this refusal - a malformed default, `packed` on a
        // non-packable field: there is nothing to link, so the load's own
        // verdict is the fixture's and no mode is entered. A fixture the
        // reference compiler ACCEPTS may never take this path, and that is the
        // only thing this branch asserts.
        if (item.accepts) {
            std::cerr << "load refused an accepting fixture " << fixture
                      << ": " << error.message << '\n';
        }
        CHECK(!item.accepts);
        return;
    }

    easypb_schema::LinkedSchema linked;
    const bool accepted = easypb_schema::link_schema(
        set, easypb_schema::LINK_COMPLETE, linked, error);
    CHECK(accepted == item.accepts);
    if (!item.accepts) {
        CHECK(linked.files == 0);
        return;
    }

    const std::string artifact = oracle_dir + "/" + artifact_name(fixture);
    std::string bytes;
    if (!read_binary_file(artifact, bytes)) {
        std::cout << "SKIP: " << fixture << " (" << artifact << ")\n";
        ++skipped;
        return;
    }
    const FileDescriptorSet decoded = easypb::decode<FileDescriptorSet>(bytes);
    CHECK(decoded.file.size() == 1);
    if (decoded.file.size() != 1) return;
    const std::string logical = text(decoded.file[0].name);
    const easypb_schema::SchemaFile* mine = set.find_file(logical);
    CHECK(mine != 0);
    if (mine == 0) return;
    compare_messages(mine->file.message_type, decoded.file[0].message_type,
                     logical);
    ++compared;
}

// unsupported_syntax/ - A25's source half, and the one corpus fixture
// deliberately OUTSIDE the differential table: protoc 30.1 ACCEPTS
// `edition = "2023";` (it models `syntax: "editions"`), while plan 4 refuses
// it by decision, so the relation is an inverse and not a comparison.
//
// What is asserted is the refusal itself. MEASURED at the 2026-09-28
// registration, and re-measured 2026-10-04 after plan 5's parser branch
// (ledger D-44): the SOURCE frontend refuses the statement by name -
// `editions syntax is not supported: only "proto2" and "proto3" are accepted`
// at the `edition` line - so the pack's 8.1 phrase "with a message naming
// editions" holds on this origin. Before that branch the answer was the generic
// `expected a top-level .proto statement`, because the parser had no `edition`
// statement while A25's own code path (DIAGNOSTIC_UNSUPPORTED_SYNTAX) is the
// DESCRIPTOR-origin rule. The refusal stays a load-stage one, so its code is
// still DIAGNOSTIC_GENERIC; the message is what names editions.
void test_editions_fixture_is_refused(const std::string& corpus)
{
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(corpus + "/unsupported_syntax");
    const std::vector<std::string> roots(1, std::string("editions.proto"));
    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, set, error));
    CHECK(!error.message.empty());
    std::cout << "INVERSE: unsupported_syntax/editions.proto refused - "
              << error.message.substr(0, 120) << '\n';
}

void test_differential_against_protoc(const std::string& corpus,
                                      const std::string& oracle_dir)
{
    std::size_t compared = 0;
    std::size_t skipped = 0;
    const std::size_t cases =
        sizeof(kDifferentialCases) / sizeof(kDifferentialCases[0]);
    for (std::size_t i = 0; i < cases; ++i) {
        run_differential_case(corpus, oracle_dir, kDifferentialCases[i],
                              compared, skipped);
    }
    // The coverage marker prompts/impl-08-fixtures-diff.md prints, and the one
    // line `matrix.py --oracle` classifies a verbose log by: a run that linked
    // without ever reading the artifacts must not look like a pass.
    std::cout << "ORACLE: compared " << compared << " fixture(s)";
    if (skipped != 0) std::cout << " (" << skipped << " skipped)";
    std::cout << '\n';
}

// ---------------------------------------------------------------------------
// Step 9 - the non-owning contract on the source path (A12)
//
// Both checks below need load_source_files, which is why they live here and
// not in test_linker.cpp: that file links easypb_schema alone and must keep
// running in the parser-free profile.
// ---------------------------------------------------------------------------

bool visible_to(const easypb_schema::LinkedSchema& linked,
                const easypb_schema::SchemaFile& file,
                const easypb_schema::SchemaFile& candidate)
{
    const std::map<const easypb_schema::SchemaFile*,
                   std::set<const easypb_schema::SchemaFile*> >::const_iterator
        found = linked.visible_files.find(&file);
    if (found == linked.visible_files.end()) return false;
    return found->second.count(&candidate) == 1;
}

// O1/O5 (Step 9): load_source_files parses each file through a temporary source
// buffer, which it destroys before returning, and every name it keeps is
// interned into the CALLER'S set. This test takes the ownership question one
// step past that guarantee: the loader options - the last object holding the
// input paths - are destroyed after the load and before the link, so by the
// time the view is read nothing that took part in reading the sources exists
// any more. Names, visibility sets and resolved fields must all still read
// correctly off the set alone.
void test_buffers_and_options_destroyed_before_the_link(
    const std::string& directory)
{
    easypb_schema::SchemaSet set;
    {
        easypb_proto::SourceLoaderOptions options;
        options.proto_paths.push_back(directory);
        const std::vector<std::string> roots = { "state.proto",
                                                "report.proto" };
        easypb_schema::Diagnostic load_error;
        CHECK(easypb_proto::load_source_files(options, roots, set, load_error));
        options.proto_paths.clear();
    }

    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                     linked, error));
    CHECK(linked.files == &set);
    CHECK(linked.complete);

    easypb_schema::SchemaFile* const report = set.find_file("report.proto");
    easypb_schema::SchemaFile* const state = set.find_file("state.proto");
    CHECK(report != 0);
    CHECK(state != 0);
    CHECK(linked.visible_files.size() == 2);
    CHECK(linked.visible_files.count(report) == 1);
    CHECK(linked.visible_files.count(state) == 1);
    CHECK(visible_to(linked, *report, *report));
    CHECK(visible_to(linked, *report, *state));
    CHECK(!visible_to(linked, *state, *report));

    // The Task 2 pair, read after every source buffer is gone: both fields
    // resolve to the imported enum, spelled absolutely.
    CHECK(report->file.message_type.size() == 1);
    CHECK(report->file.message_type[0].field.size() == 2);
    CHECK(text(report->file.message_type[0].field[0].type_name) ==
          ".shared.State");
    CHECK(report->file.message_type[0].field[0].type ==
          FieldDescriptorProto::TYPE_ENUM);
    CHECK(text(report->file.message_type[0].field[1].type_name) ==
          ".shared.State");
    CHECK(report->file.message_type[0].field[1].type ==
          FieldDescriptorProto::TYPE_ENUM);
    CHECK(text(state->file.name) == "state.proto");
    CHECK(linked.symbols.symbols.count(".shared.State") == 1);

    std::cout << "LIFETIME: names, visibility sets and resolved fields read "
                 "after the loader options were destroyed\n";
}

// O5 (Step 9): the positions recorded while parsing belong to the CALLER'S set
// (field_sources and declaration_locations), not to the buffer the token was
// read from, so a linked field still reports its own line and column once every
// source buffer is gone. The numbers are the ones Step 8.2 measures on this
// fixture - report.proto's two type tokens are at 10:12 and 11:12 - and naming
// them is what the pack requires instead of "not (0,0)": a parser that reported
// the statement's start or unknown_location() fails here.
void test_real_locations_survive_the_sources_going_away(
    const std::string& directory)
{
    easypb_schema::SchemaSet set;
    {
        easypb_proto::SourceLoaderOptions options;
        options.proto_paths.push_back(directory);
        const std::vector<std::string> roots = { "state.proto",
                                                "report.proto" };
        easypb_schema::Diagnostic load_error;
        CHECK(easypb_proto::load_source_files(options, roots, set, load_error));
        options.proto_paths.clear();
    }

    easypb_schema::LinkedSchema linked;
    easypb_schema::Diagnostic error;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE,
                                     linked, error));

    easypb_schema::SchemaFile* const report = set.find_file("report.proto");
    CHECK(report != 0);
    CHECK(report->field_sources.size() == 2);
    CHECK(report->field_sources[0].type_location.line == 10);
    CHECK(report->field_sources[0].type_location.column == 12);
    CHECK(report->field_sources[1].type_location.line == 11);
    CHECK(report->field_sources[1].type_location.column == 12);
    // The descriptor path the same entry carries, which is what the diagnostic
    // side falls back to when there is no position at all: the two halves agree
    // about which field they describe.
    CHECK(report->field_sources[0].path.size() == 4);
    CHECK(report->field_sources[0].path[0] == 4);
    CHECK(report->field_sources[0].path[1] == 0);
    CHECK(report->field_sources[0].path[2] == 2);
    CHECK(report->field_sources[0].path[3] == 0);
    CHECK(report->field_sources[1].path[3] == 1);
    // The declaration map is not field-only - it also carries the file's
    // package and message declarations - so the check names the two field
    // paths rather than guessing a total.
    CHECK(report->declaration_locations.count(
              report->field_sources[0].path) == 1);
    CHECK(report->declaration_locations.count(
              report->field_sources[1].path) == 1);

    std::cout << "LOCATIONS: report.proto 10:12 and 11:12 after the sources "
                 "were destroyed\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: codegen_imports_source_linker_test "
                     "<fixture-dir> [oracle-dir]\n";
        return EXIT_FAILURE;
    }
    const std::string data_dir(argv[1]);
    // tests/CMakeLists.txt passes build/package-imports/oracle; without it every
    // accepting fixture reports SKIP and the run still succeeds, which is what
    // Step 8.3 requires of a machine that has not generated the sets.
    const std::string oracle_dir = argc > 2 ? std::string(argv[2]) : std::string();

    test_relative_references_match_protoc(data_dir + "/relative");
    test_service_head_cases_match_protoc(data_dir);
    test_source_locations_point_at_the_type_token(data_dir + "/relative");
    test_editions_fixture_is_refused(data_dir);
    test_differential_against_protoc(data_dir, oracle_dir);
    test_buffers_and_options_destroyed_before_the_link(data_dir);
    test_real_locations_survive_the_sources_going_away(data_dir);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all source linker tests passed\n";
    return EXIT_SUCCESS;
}
