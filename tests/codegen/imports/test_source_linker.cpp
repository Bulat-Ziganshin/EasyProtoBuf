// Source-loader driven tests for the linked-schema layer: real .proto text in
// tests/codegen/imports/data/linker, loaded through load_source_files and then
// linked with link_schema. This is the home of the parser-dependent lifetime
// and source-location tests; the parser-free subset lives in test_linker.cpp.
//
// Registered only inside if(EASYPB_CODEGEN_WITH_PROTO_PARSER) in
// tests/CMakeLists.txt, so descriptor-set-only profiles never compile it.

#include <cstdlib>
#include <iostream>
#include <string>

#include "proto_loader.hpp"
#include "schema_linker.hpp"

int main(int argc, char** argv)
{
    // Step 1 skeleton. It still reads the data directory the CTest
    // registration passes as a positional argument and fails without it:
    // that is the only thing this step can verify about the source-driven
    // wiring, and a later edit that drops the argument would otherwise stay
    // invisible until Step 5. Step 5 fills this executable in.
    if (argc < 2) {
        std::cerr << "usage: codegen_imports_source_linker_test <fixture-dir>\n";
        return EXIT_FAILURE;
    }

    std::cout << "source linker skeleton: no cases yet (fixtures: "
              << argv[1] << ")\n";
    return EXIT_SUCCESS;
}
