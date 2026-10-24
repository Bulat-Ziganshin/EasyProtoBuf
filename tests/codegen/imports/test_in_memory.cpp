// Plan 5's in-memory lane: the common adapter and the linked generator
// driven through easypb_codegen_core alone - no CLI, no descriptor-file
// wrapper, no native file helper, no publisher. The test binary decodes two
// committed `.pbs` containers in memory, ingests every file through the owned
// adapter, poisons and discards the only buffers the borrowed views pointed
// into, selects targets in an order different from the containers' order,
// links and generates - and every generated name and byte must equal what the
// standalone CLI published for the same containers (the strict `.pbs` output,
// prepared by test_in_memory.cmake).
//
// The runner starts this binary from an empty working directory (T4): the
// current directory must contribute nothing to the in-memory path, and it
// must stay empty afterwards. The link closure is the fixture for the
// "runs with the core only" half of the same invariant: linking this target
// needs easypb_codegen_core and easypb_schema, and nothing else.
//
// Invariants witnessed (build/plan5-codegen-integration-process/
// invariants.md): T4 (core-only link, empty directory), plus the plan's
// exit-criteria rows 1-2 for `codegen.imports.in_memory`.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "descriptor_adapter.hpp"
#include "generation.hpp"
#include "generation_options.hpp"
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

// Reads one fixture whole, in binary mode: generated-header comparison must
// not normalise line endings or anything else.
bool read_binary_file(const std::string& path, std::string& contents)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    contents.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

// Fills the buffer with a pattern no generated header may legitimately
// contain, so a surviving view into it cannot read what it used to.
void poison(std::string& buffer)
{
    if (!buffer.empty()) {
        std::memset(&buffer[0], '\xA5', buffer.size());
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3 || argv[1][0] == '\0' || argv[2][0] == '\0') {
        std::cerr << "usage: test_in_memory <tests-source-dir> "
                     "<strict-output-dir>\n"
                  << "The strict output directory holds the CLI's published\n"
                  << "headers for the same containers, prepared by the ctest\n"
                  << "driver (test_in_memory.cmake).\n";
        return EXIT_FAILURE;
    }
    const std::string fixture_dir = argv[1];
    const std::string strict_dir = argv[2];

    easypb_schema::SchemaSet set;
    easypb_schema::Diagnostic error;
    {
        // The two only buffers this run ever reads: one complete three-file
        // project set and one standalone two-file set.
        std::string project_bytes;
        std::string multi_bytes;
        CHECK(read_binary_file(
            fixture_dir + "/codegen/imports/data/project.pbs", project_bytes));
        CHECK(read_binary_file(
            fixture_dir + "/codegen/parser/data/integration-multi.pbs",
            multi_bytes));
        {
            const FileDescriptorSet project =
                easypb::decode<FileDescriptorSet>(project_bytes);
            const FileDescriptorSet multi =
                easypb::decode<FileDescriptorSet>(multi_bytes);
            CHECK(project.file.size() == 3);
            CHECK(multi.file.size() == 2);
            CHECK(easypb_schema::append_descriptor_files(
                project.file, "project.pbs", set, error));
            CHECK(easypb_schema::append_descriptor_files(
                multi.file, "integration-multi.pbs", set, error));
        }
        // Both borrowed views die here, twice over.
        poison(project_bytes);
        poison(multi_bytes);
        project_bytes.clear();
        project_bytes.shrink_to_fit();
        multi_bytes.clear();
        multi_bytes.shrink_to_fit();
    }

    CHECK(set.files().size() == 5);
    CHECK(set.targets().empty());

    // Selection is explicit and in an order the containers do not use: the
    // project's report comes after the standalone set's proto3 file in the
    // request, while the containers hold the project files first. Imported
    // files (both common dependencies) are loaded but must not become
    // implicit targets.
    easypb_schema::SchemaFile* report = set.find_file("api/report.proto");
    easypb_schema::SchemaFile* proto3 = set.find_file("integration-proto3.proto");
    CHECK(report != 0);
    CHECK(proto3 != 0);
    if (report == 0 || proto3 == 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    set.add_target(*report);
    set.add_target(*proto3);
    CHECK(set.targets().size() == 2);
    CHECK(set.targets()[0] == report);
    CHECK(set.targets()[1] == proto3);

    // Linking and generation run with every borrowed buffer gone.
    easypb_schema::LinkedSchema linked;
    CHECK(easypb_schema::link_schema(set, easypb_schema::LINK_COMPLETE, linked,
                                     error));
    CHECK(linked.local_only);

    easypb_codegen::GenerationOptions options;
    std::string option_error;
    CHECK(easypb_codegen::finalize_generation_options(options, option_error));

    std::vector<easypb_codegen::GeneratedFile> files;
    CHECK(easypb_codegen::generate_files(linked, options, files, error));
    CHECK(files.size() == 2);
    if (files.size() == 2) {
        CHECK(files[0].name == "api/report.pb.hpp");
        CHECK(files[1].name == "integration-proto3.pb.hpp");
        // The report header keeps its dependency includes and its imported
        // nonzero default; both are only possible from the owned copy.
        CHECK(files[0].content.find("// Source: api/report.proto") !=
              std::string::npos);
        CHECK(files[0].content.find("#include \"common/state.pb.hpp\"") !=
              std::string::npos);
        CHECK(files[0].content.find(
                  "::shared::State state = ::shared::State::READY;") !=
              std::string::npos);

        // The strict `.pbs` reference: byte-for-byte what the CLI published
        // for the same containers and the same selection.
        std::string strict_report;
        std::string strict_proto3;
        CHECK(read_binary_file(strict_dir + "/api/report.pb.hpp",
                               strict_report));
        CHECK(read_binary_file(strict_dir + "/integration-proto3.pb.hpp",
                               strict_proto3));
        CHECK(files[0].content == strict_report);
        CHECK(files[1].content == strict_proto3);
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all in-memory generation tests passed\n";
    return EXIT_SUCCESS;
}
