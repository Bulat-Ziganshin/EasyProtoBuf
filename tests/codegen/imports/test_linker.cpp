// Tests for the linked-schema layer (codegen/schema_linker.*). This file
// links only easypb_schema -- no parser, no loader -- because it must keep
// passing in descriptor-set-only builds (EASYPB_CODEGEN_WITH_PROTO_PARSER=OFF).
// That single constraint decides which cases can live here: descriptor-only
// isolation, set growth, repeated runs, and pool growth do; anything that
// destroys a source buffer or asserts a real source location must not, and
// belongs in test_source_linker.cpp instead. Fixtures are in-memory
// descriptors with empty physical_name and unknown positions.

#include <cstdlib>
#include <iostream>

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

// Step 1 skeleton test: linking is not implemented yet, and the stub must
// fail loudly instead of publishing an empty result as a successful link.
// Step 2.2 replaces this expectation with the staged publish rule and Step 5
// with real success, so this is the one case that is meant to be deleted.
void test_link_stub_reports_not_implemented()
{
    easypb_schema::SchemaSet files;
    easypb_schema::LinkedSchema result;
    easypb_schema::Diagnostic error;

    const bool linked = easypb_schema::link_schema(
        files, easypb_schema::LINK_COMPLETE, result, error);

    CHECK(!linked);
    CHECK(error.code == easypb_schema::DIAGNOSTIC_GENERIC);
    CHECK(error.message == "link_schema: not implemented");
    CHECK(result.files == 0);          // invalid empty state: never published
    CHECK(!result.complete);           // no verdict was earned
    CHECK(result.symbols.symbols.empty());
    CHECK(result.visible_files.empty());
}

} // namespace

int main()
{
    test_link_stub_reports_not_implemented();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all schema linker tests passed\n";
    return EXIT_SUCCESS;
}
