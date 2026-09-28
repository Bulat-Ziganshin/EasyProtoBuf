// Step 11's two-way oracle parity lane, the LINKER half: one CTest row
// (`codegen.imports.parity`) that reads
// tests/codegen/imports/data/parity/parity-cases.tsv and asserts, for every
// row, the verdict this repository promises beside the verdict libprotoc 30.1
// was measured to give.
//
// The lane exists because plan 4's refusals are settled against protoc, and
// until now that evidence was per-finding and lives under a gitignored
// directory (build/plan4/evidence/oracle/VERDICTS.md, eight hand-written
// drivers, no test invoking any of them). The step's text:
// plan4/steps/06-oracle-parity.md; the measurement this lane is born from:
// build/plan4/oracle-parity/parity-verdicts.txt.
//
// What a row is (§11.1). One row per fixture, tab-separated, six columns:
// `case`, `origin`, `protoc`, `expected`, `kind`, `note`. The row's *code*, if
// it has one, is named in `expected` as the enumerator
// (`DIAGNOSTIC_EMPTY_ONEOF`) or is `accept`. A code's accept column is the row
// `<origin>-<slug>-accept`, where `<slug>` is the enumerator minus the
// `DIAGNOSTIC_` prefix, lower-cased with `_` turned into `-` - so the pairing
// is a property of the DATA, and this file checks that rule against the tsv
// itself instead of carrying a second table (a copy is the one that goes
// stale, and §11.8 forbids a second table of the same facts).
//
// VERDICTS, NEVER MESSAGES. Nothing here asserts protoc's wording or ours: the
// project deliberately does not follow protoc's sentences (E310, W13/W14,
// B8/B9/B12/B14/B15), because E1/A3 make the code, not the text, the contract.
// A row that starts asserting text is out of scope by §11.6.
//
// The two modes (§11.4). Every row is linked twice, because the plan's promise
// is per mode and fix round r13's fix was a both-modes fix. A row is
// *compat-witnessable* when its set is exactly one descriptor-origin file; the
// declared verdict is then asserted in BOTH modes. Every other row (several
// files, source origin, or a refusal at the load stage) cannot be witnessed by
// compatibility mode at all: A26 makes the mode's own precondition refuse
// "exactly one descriptor-origin file", so what is asserted there is that
// refusal - DIAGNOSTIC_GENERIC, the mode's own verdict, with nothing published.
// A mode precondition is not a parity verdict, and this is the difference
// recorded rather than papered over.
//
// TWO ROWS, ONE FILE, ONE TABLE. Source-origin rows go through the loader's
// own path (§11.4), and the package's A12 split makes "parser-free" and
// "parser-dependent" mutually exclusive target classes: `verify-package.py`'s
// `iso:parser-free targets link nothing the parser owns` fails any target that
// links `libeasypb_schema.a` alone in a parser-off tree and the loader in a
// parser-on one. So the lane is two registrations of this same file:
//
//   codegen.imports.parity        the descriptor-origin rows, `easypb_schema`
//                                 alone, outside the parser guard, so it exists
//                                 in every profile - the checkpoint runs it in
//                                 the parser-free tree too
//   codegen.imports.parity.source the source-origin rows, through the loader,
//                                 inside the parser guard
//
// Each binary NAMES every row it does not read with the pack's `SKIP:` marker
// and the name of the row that does: a green run in which half the fixtures
// were silently never read is the failure mode that rule exists for, and the
// two halves' tallies are printed side by side so an empty half is visible.
//
// The `protoc` column is DATA here, not truth: it is the recorded verdict with
// its provenance, re-derived by the `protoc` half
// (data/parity/run.py, opt-in; §11.5).

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "schema.hpp"
#include "schema_linker.hpp"

#if defined(EASYPB_PARITY_HALF_SOURCE)
#include "proto_loader.hpp"
#endif

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

std::string text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

bool read_text_file(const std::string& path, std::string& contents)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    contents.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
    return true;
}

std::string join(const std::string& dir, const std::string& name)
{
    return dir + "/" + name;
}

// ---------------------------------------------------------------------------
// The enumerator table. Every value of DiagnosticCode is named here once, so a
// row's `expected` cell can be read into a code without the test carrying a
// second copy of the tsv's own rows. The count assertion below is what keeps
// this list honest when the enum grows: a new enumerator that no row can name
// fails the run instead of being silently uncovered.
// ---------------------------------------------------------------------------

struct CodeName {
    const char* name;
    easypb_schema::DiagnosticCode code;
};

const CodeName kCodeNames[] = {
    {"DIAGNOSTIC_GENERIC", easypb_schema::DIAGNOSTIC_GENERIC},
    {"DIAGNOSTIC_UNRESOLVED_TYPE", easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE},
    {"DIAGNOSTIC_INVALID_IMPORT", easypb_schema::DIAGNOSTIC_INVALID_IMPORT},
    {"DIAGNOSTIC_MISSING_IMPORT", easypb_schema::DIAGNOSTIC_MISSING_IMPORT},
    {"DIAGNOSTIC_IMPORT_CYCLE", easypb_schema::DIAGNOSTIC_IMPORT_CYCLE},
    {"DIAGNOSTIC_DUPLICATE_SYMBOL", easypb_schema::DIAGNOSTIC_DUPLICATE_SYMBOL},
    {"DIAGNOSTIC_INVISIBLE_TYPE", easypb_schema::DIAGNOSTIC_INVISIBLE_TYPE},
    {"DIAGNOSTIC_NOT_A_TYPE", easypb_schema::DIAGNOSTIC_NOT_A_TYPE},
    {"DIAGNOSTIC_MAP_ENTRY_TYPE", easypb_schema::DIAGNOSTIC_MAP_ENTRY_TYPE},
    {"DIAGNOSTIC_KIND_MISMATCH", easypb_schema::DIAGNOSTIC_KIND_MISMATCH},
    {"DIAGNOSTIC_MALFORMED_MAP_ENTRY",
     easypb_schema::DIAGNOSTIC_MALFORMED_MAP_ENTRY},
    {"DIAGNOSTIC_UNSUPPORTED_SYNTAX",
     easypb_schema::DIAGNOSTIC_UNSUPPORTED_SYNTAX},
    {"DIAGNOSTIC_INVALID_FIELD_NUMBER",
     easypb_schema::DIAGNOSTIC_INVALID_FIELD_NUMBER},
    {"DIAGNOSTIC_MISSING_TYPE_NAME",
     easypb_schema::DIAGNOSTIC_MISSING_TYPE_NAME},
    {"DIAGNOSTIC_DUPLICATE_ENUM_NUMBER",
     easypb_schema::DIAGNOSTIC_DUPLICATE_ENUM_NUMBER},
    {"DIAGNOSTIC_EMPTY_ONEOF", easypb_schema::DIAGNOSTIC_EMPTY_ONEOF},
    {"DIAGNOSTIC_EMPTY_ENUM", easypb_schema::DIAGNOSTIC_EMPTY_ENUM},
    {"DIAGNOSTIC_PROTO3_REQUIRED_FIELD",
     easypb_schema::DIAGNOSTIC_PROTO3_REQUIRED_FIELD},
    {"DIAGNOSTIC_PROTO3_EXPLICIT_DEFAULT",
     easypb_schema::DIAGNOSTIC_PROTO3_EXPLICIT_DEFAULT},
};

const std::size_t kCodeNameCount = sizeof(kCodeNames) / sizeof(kCodeNames[0]);

// The tsv's `expected` cell for one of these; `found` reports whether the name
// is one of the enum's.
easypb_schema::DiagnosticCode code_from_name(const std::string& name,
                                            bool& found)
{
    for (std::size_t i = 0; i < kCodeNameCount; ++i) {
        if (name == kCodeNames[i].name) {
            found = true;
            return kCodeNames[i].code;
        }
    }
    found = false;
    return easypb_schema::DIAGNOSTIC_GENERIC;
}

// `DIAGNOSTIC_EMPTY_ONEOF` -> `empty-oneof`: the fixture naming rule of
// data/parity/README.md, and the reason a code's accept column is findable.
std::string slug_of(const std::string& code_name)
{
    const std::string prefix("DIAGNOSTIC_");
    std::string name = code_name.compare(0, prefix.size(), prefix) == 0
                           ? code_name.substr(prefix.size())
                           : code_name;
    std::string slug;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '_') {
            slug += '-';
        } else {
            slug += static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return slug;
}

struct Row {
    std::string name;
    std::string origin;
    std::string protoc;
    std::string expected;
    std::string kind;
    std::string note;
    std::size_t line;
};

std::vector<std::string> split(const std::string& line, char separator)
{
    std::vector<std::string> fields;
    std::string current;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == separator) {
            fields.push_back(current);
            current.clear();
        } else {
            current += line[i];
        }
    }
    fields.push_back(current);
    return fields;
}

const char* const kHeader =
    "case\torigin\tprotoc\texpected\tkind\tnote";

bool read_rows(const std::string& path, std::vector<Row>& rows)
{
    std::string contents;
    if (!read_text_file(path, contents)) {
        std::cerr << "cannot read " << path << '\n';
        return false;
    }
    std::size_t line_number = 0;
    std::size_t at = 0;
    while (at <= contents.size()) {
        std::size_t end = contents.find('\n', at);
        if (end == std::string::npos) end = contents.size();
        std::string line = contents.substr(at, end - at);
        at = end + 1;
        ++line_number;
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        if (line.empty()) continue;
        if (rows.empty() && line == kHeader) continue;
        const std::vector<std::string> fields = split(line, '\t');
        if (fields.size() != 6) {
            std::cerr << path << ':' << line_number << ": expected 6 tab-separated"
                      << " fields, found " << fields.size() << '\n';
            return false;
        }
        Row row;
        row.name = fields[0];
        row.origin = fields[1];
        row.protoc = fields[2];
        row.expected = fields[3];
        row.kind = fields[4];
        row.note = fields[5];
        row.line = line_number;
        rows.push_back(row);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The tsv's own consistency: the rules §11.1 and §11.3 state, checked against
// the data rather than trusted.
// ---------------------------------------------------------------------------

void check_table(const std::vector<Row>& rows)
{
    // One entry per `DiagnosticCode` value (fix round r17 moved 17 -> 19), so a
    // value appended to the enum cannot be missing from the name table the tsv
    // rows are resolved through.
    CHECK(kCodeNameCount == 19);
    CHECK(!rows.empty());

    std::set<std::string> names;
    std::map<std::string, std::size_t> code_rows;
    std::size_t none_rows = 0;

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const std::string where = "row " + row.name;
        CHECK(names.insert(row.name).second);
        CHECK(row.origin == "source" || row.origin == "descriptor");

        const bool protoc_accept = row.protoc == "accept";
        CHECK(protoc_accept || row.protoc == "reject");
        const bool expected_accept = row.expected == "accept";
        bool known = false;
        const easypb_schema::DiagnosticCode code =
            code_from_name(row.expected, known);
        if (!expected_accept) {
            // The cell is a code name, and never DIAGNOSTIC_GENERIC: value 0 is
            // not a parity code, it is the bucket for the loader/CLI classes
            // and the value an accept leaves untouched (§11.6).
            CHECK(known);
            CHECK(!known || code != easypb_schema::DIAGNOSTIC_GENERIC);
            if (known) ++code_rows[row.expected];
        }

        if (row.kind == "pair") {
            // Both directions agree: an accept row is one protoc accepts, a
            // refuse row is one protoc refuses.
            CHECK(protoc_accept == expected_accept);
            CHECK(row.note.empty());
        } else if (row.kind == "inverse") {
            // protoc accepts, the linker refuses BY DECISION, and the decision
            // is recorded in the note.
            CHECK(protoc_accept && !expected_accept);
            CHECK(!row.note.empty());
        } else if (row.kind == "none") {
            // No counterpart can exist, with the reason in the note: the
            // negative row, asserted over every accepted fixture.
            CHECK(protoc_accept && expected_accept);
            CHECK(!row.note.empty());
            ++none_rows;
        } else {
            CHECK(false && "unknown row kind");
        }
    }

    // The accept column (§11.1): every row that names a code must have its
    // accept twin - an input protoc accepts, which the linker must also accept,
    // asserting that the code is NOT raised for it. Without this half a rule
    // that over-refuses passes every refusal-only fixture (fix round r13 is the
    // worked example: only an accept control keeps the intentional enum alias
    // in tests/codegen/enums/data/enum_codegen.proto linking).
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        if (row.expected == "accept") continue;
        const std::string twin = std::string(1, row.origin[0]) + "-" +
                                 slug_of(row.expected) + "-accept";
        CHECK(names.find(twin) != names.end());
    }

    // Every enumerator is covered: a code has at least one row naming it, and
    // DIAGNOSTIC_GENERIC - which no row may name - is covered by the `none`
    // row instead (§11.6).
    for (std::size_t i = 0; i < kCodeNameCount; ++i) {
        const std::string name = kCodeNames[i].name;
        if (kCodeNames[i].code == easypb_schema::DIAGNOSTIC_GENERIC) {
            CHECK(code_rows.find(name) == code_rows.end());
        } else {
            CHECK(code_rows.find(name) != code_rows.end());
        }
    }
    CHECK(none_rows == 1);
}

// ---------------------------------------------------------------------------
// Running one row.
// ---------------------------------------------------------------------------

// Which half of the table this binary reads. A standalone build (neither
// define present) runs the whole lane, which is what the step's own probe
// builds do.
#if defined(EASYPB_PARITY_HALF_SOURCE)
const char* const kHalf = "source";
#elif defined(EASYPB_PARITY_HALF_DESCRIPTOR)
const char* const kHalf = "descriptor";
#else
const char* const kHalf = "both";
#endif

// Table-wide tallies. `complete_generic_refusals` is the negative row of
// DIAGNOSTIC_GENERIC (§11.6) done as a property instead of a paragraph: value
// 0 is the bucket for the loader/CLI classes and is never a parity verdict, so
// no row may be refused with it. Compatibility mode's own precondition is the
// single place it legitimately appears, and `compat_preconditions` is asserted
// against the rows that mode cannot witness.
std::size_t complete_generic_refusals = 0;
std::size_t compat_preconditions = 0;
std::size_t compat_witnessable_rows = 0;
std::size_t load_refusals = 0;
std::size_t skipped_rows = 0;

void run_row(const std::string& directory, const Row& row)
{
    std::cout << "PARITY: " << row.name << " origin=" << row.origin
              << " protoc=" << row.protoc << " expected=" << row.expected
              << '\n';

    const bool expected_accept = row.expected == "accept";
    bool known = false;
    const easypb_schema::DiagnosticCode expected_code =
        code_from_name(row.expected, known);
    CHECK(expected_accept || known);

    easypb_schema::SchemaSet set;
    std::string bytes;

    if (row.origin == "descriptor") {
        const std::string path =
            join(join(directory, "set"), row.name + ".pbs");
        CHECK(read_text_file(path, bytes));
        const FileDescriptorSet decoded = easypb::decode<FileDescriptorSet>(
            bytes);
        for (std::size_t i = 0; i < decoded.file.size(); ++i) {
            const std::string logical = text(decoded.file[i].name);
            easypb_schema::SchemaFile& file = set.add_file(logical);
            file.physical_name.clear();
            file.from_descriptor_set = true;
            // The decoded views point into `bytes`, which outlives the link in
            // this scope; C++11 copies the str_views into the descriptor.
            file.file = decoded.file[i];
        }
    } else {
#if defined(EASYPB_PARITY_HALF_SOURCE)
        easypb_proto::SourceLoaderOptions options;
        options.proto_paths.push_back(join(directory, "source"));
        // The fixture FILE is `<case>.proto`; the case name is its stem.
        const std::vector<std::string> roots(1, row.name + ".proto");
        easypb_schema::Diagnostic error;
        if (!easypb_proto::load_source_files(options, roots, set, error)) {
            // The loader owns the refusal: one of the import classes is decided
            // before any link runs, and the code it answers with is the row's
            // verdict. There is nothing to link, so no mode is entered - a mode
            // is a property of a link that happened.
            ++load_refusals;
            CHECK(!expected_accept);
            if (!expected_accept) CHECK(error.code == expected_code);
            std::cout << "PARITY: " << row.name
                      << " stage=load verdict=REJECT code="
                      << static_cast<int>(error.code) << '\n';
            return;
        }
#else
        ++skipped_rows;
        // Unreachable through either registration: the source rows are
        // filtered to the gated half in main, and this branch only runs for a
        // binary built with neither define, which is a build mistake.
        CHECK(false && "source-origin row in a binary built without the loader");
        ++skipped_rows;
        std::cout << "SKIP: " << row.name
                  << " source-origin row needs the loader "
                     "(EASYPB_PARITY_HALF_SOURCE)\n";
        return;
#endif
    }

    std::size_t files = 0;
    bool all_descriptor_origin = true;
    for (std::size_t i = 0; i < set.files().size(); ++i) {
        ++files;
        if (!set.files()[i]->from_descriptor_set) all_descriptor_origin = false;
    }
    const bool compat_witnessable = files == 1 && all_descriptor_origin;

    // 1. Complete mode: the parity verdict the row declares, in full.
    {
        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        const bool accepted = easypb_schema::link_schema(
            set, easypb_schema::LINK_COMPLETE, linked, error);
        CHECK(accepted == expected_accept);
        // A1: a refusal publishes nothing.
        if (!accepted) CHECK(linked.files == 0);
        if (accepted) {
            CHECK(linked.files == &set);
        } else {
            CHECK(known);
            if (known) CHECK(error.code == expected_code);
            if (error.code == easypb_schema::DIAGNOSTIC_GENERIC) {
                ++complete_generic_refusals;
            }
        }
        std::cout << "PARITY: " << row.name << " mode=complete verdict="
                  << (accepted ? "ACCEPT" : "REJECT") << " code="
                  << static_cast<int>(error.code) << '\n';
    }

    // 2. Compatibility mode: the same verdict where the mode can witness it,
    //    its own precondition otherwise (see the file header).
    {
        easypb_schema::LinkedSchema linked;
        easypb_schema::Diagnostic error;
        const bool accepted = easypb_schema::link_schema(
            set, easypb_schema::LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY, linked,
            error);
        if (compat_witnessable) {
            ++compat_witnessable_rows;
            CHECK(accepted == expected_accept);
            if (!accepted) CHECK(linked.files == 0);
            if (accepted) CHECK(linked.files == &set);
            if (!accepted && known) CHECK(error.code == expected_code);
        } else {
            ++compat_preconditions;
            CHECK(!accepted);
            CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
            CHECK(linked.files == 0);
        }
        std::cout << "PARITY: " << row.name << " mode=compat verdict="
                  << (accepted ? "ACCEPT" : "REJECT") << " code="
                  << static_cast<int>(error.code)
                  << (compat_witnessable ? "" : " (mode precondition)") << '\n';
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argv[1][0] == '\0') {
        std::cerr << "usage: codegen_imports_parity_test <tests source dir>\n";
        return 2;
    }
    const std::string directory =
        join(join(argv[1], "codegen/imports/data"), "parity");

    std::vector<Row> rows;
    if (!read_rows(join(directory, "parity-cases.tsv"), rows)) return 1;
    check_table(rows);
    if (failures != 0) {
        std::cerr << failures << " table failure(s): no fixture was read\n";
        return 1;
    }

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        if (std::string(kHalf) != "both" && row.origin != kHalf) {
            ++skipped_rows;
            std::cout << "SKIP: " << row.name << " origin=" << row.origin
                      << " - the " << row.origin
                      << "-origin half is read by "
                      << (row.origin == "source"
                              ? "codegen.imports.parity.source"
                              : "codegen.imports.parity")
                      << "\n";
            continue;
        }
        run_row(directory, row);
    }

    std::size_t accept_rows = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].expected == "accept") ++accept_rows;
    }
    std::cout << "PARITY: half=" << kHalf << " compared "
              << rows.size() - skipped_rows << " of " << rows.size()
              << " row(s), " << accept_rows
              << " accept controls in the table\n";
    // The run's own accounting: every row is classified exactly once, and the
    // negative row of DIAGNOSTIC_GENERIC holds table-wide.
    std::cout << "PARITY: generic-negative complete_refusals_with_GENERIC="
              << complete_generic_refusals << " compat_witnessed="
              << compat_witnessable_rows << " compat_preconditions="
              << compat_preconditions << " load_refusals=" << load_refusals
              << " skipped=" << skipped_rows << '\n';
    CHECK(complete_generic_refusals == 0);
    CHECK(compat_witnessable_rows + compat_preconditions + load_refusals +
              skipped_rows == rows.size());
    if (failures != 0) {
        std::cerr << failures << " parity failure(s)\n";
        return 1;
    }
    return 0;
}
