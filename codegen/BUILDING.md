# Building and testing EasyProtoBuf Codegen

The modern command-line examples below require CMake 3.20 or newer. They set `CMAKE_BUILD_TYPE` for single-configuration generators and also pass `--config` / `-C` for multi-configuration generators such as Visual Studio, so both build styles consistently use `Release`.

## Building

From the repository root, build the project with CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The CMake target is named `easypb_codegen` internally and produces the `codegen` executable.

The build separates the pieces so each consumer links only what it needs: `easypb_codegen_core` (the linked generator, options and pure header-path mapping), `easypb_schema` (schema storage, semantics, linking and the descriptor adapter), `easypb_file_io` (native path rules and reads/writes), `easypb_descriptor_loader` (the standalone `.pbs` container reader) and `easypb_header_output` (the output publisher). The core links the schema library only; a caller that generates in memory never links the CLI, the publisher or any filesystem helper.

With xmake:

```sh
xmake
```

## Optional descriptor-set-only build

If only descriptor-set input is needed, parser support can be omitted at build time:

```sh
cmake -S . -B build-lite -DCMAKE_BUILD_TYPE=Release -DEASYPB_CODEGEN_WITH_PROTO_PARSER=OFF
cmake --build build-lite --config Release
```

With xmake:

```sh
xmake f --codegen_parser=n
xmake
```

In this configuration nothing from [`parser/`](parser/) is compiled. Codegen accepts descriptor sets through either `--descriptor-set file.pbs` or the implicit `.pbs` form.

## Testing

From the repository root, the normal CMake test suite exercises both input formats and the parser utilities:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The [`codegen.modes`](../tests/codegen/parser/test_codegen_modes.cmake) test checks explicit and implicit descriptor-set input, `.proto` versus `.pbs` generated-code equivalence, proto2/proto3 packed behavior, descriptor printing, parser benchmarking, missing-import handling, and invalid empty descriptor sets.

The plan-5 integration suites cover the new surface. [`codegen.imports.cli`](../tests/codegen/cli/test_codegen_cli.cmake) drives the mode table, option validation, stdout cardinality, non-ASCII output roots and publication snapshots through the built executable; the plan-5 follow-up's S4 rows extend the same lane with the option grammar (unknown flags, `--`, missing values), the non-ASCII read paths, the generation path's warnings, the anonymous-descriptor identity and the order-independent publication of a case-only target pair; [`codegen.imports.output`](../tests/codegen/imports/test_output.cpp) stages the publisher's path/identity matrix and an injected later-file failure; [`codegen.imports.includes`](../tests/codegen/imports/test_includes.cmake) pins the support-import include rule with its real-reference and public-re-export counterexamples; [`codegen.imports.in_memory`](../tests/codegen/imports/test_in_memory.cpp) links the generation core alone, runs from an empty directory and must byte-match the CLI's `.pbs` output; [`codegen.imports.runtime`](../tests/codegen/imports/runtime/two_tu_main.cpp) compiles independently generated headers in two translation units and round-trips an imported enum default, a packed enum, a nested message and a message-valued map.

The [`codegen.limits`](../tests/codegen/limits/test_limits.cmake) test covers the boundaries of untrusted input: the nesting limit (the 31 levels the reference compiler accepts and the 32 it refuses, on both input paths, with the accepted header compiled and its data checked), and the bounded output tree (a logical name with more components than the limit is refused before anything is created). [`codegen.imports.output`](../tests/codegen/imports/test_output.cpp) carries the library half: the publisher's own name check and the iterative, bounded directory creation.

The [`codegen.maps`](../tests/codegen/maps/test_codegen_maps.cmake) test covers scalar, enum and message-valued map generation, custom map containers, malformed map descriptors, and `.proto`/`.pbs` equivalence. `codegen.maps.runtime` compiles the generated ADL codecs and verifies scalar, enum and message-valued map round trips in both full and descriptor-set-only builds.

The [`codegen.custom_types`](../tests/codegen/custom_types/test_codegen_custom_types.cmake) test covers global and per-field C++ type templates, placeholder expansion, `.proto`/`.pbs` parity, EasyProtoBuf custom-option diagnostics, descriptor extension handling, and preservation of protobuf-driven codec/dependency semantics. `codegen.custom_types.runtime` compiles representative custom containers/wrappers and verifies an encode/decode round trip.

The [`codegen.enums`](../tests/codegen/enums/test_codegen_enums.cmake) test covers top-level and nested enum declarations, fixed underlying types, qualified defaults, shadowing, aliases, negative values, and `.proto`/`.pbs` equivalence. `codegen.enums.runtime` verifies enum wire behavior, including preservation of unknown proto3 enum values.

The [`codegen.packages`](../tests/codegen/packages/test_codegen_packages.cmake) tests cover package-to-C++ namespace mapping, absolute nested/enum references, package validation, `.proto`/`.pbs` equivalence, descriptor-only external enum naming, unchanged insertion-macro spellings, same leaf names in different packages, and multi-translation-unit ADL round trips.

The nested-message checks in [`codegen.modes`](../tests/codegen/parser/test_codegen_modes.cmake) cover lexical C++ nesting, qualified names, forward-reference ordering, `.proto`/`.pbs` equivalence, default rejection of recursive value dependencies, opt-in direct self-recursive repeated/map generation, and continued rejection of unsupported recursive graphs. [`codegen.nested.runtime`](../tests/codegen/nested/nested_runtime.cpp) compiles the generated C++11 code and verifies nested-message and nested-message-map round trips.

`codegen.recursive.repeated.runtime` always compiles the default opt-in `std::vector<Self>` representation and verifies a recursive repeated-message round trip. This is intentionally unconditional: Codegen itself already requires the same pattern because [`descriptor.pb.hpp`](descriptor.pb.hpp) contains `std::vector<DescriptorProto> nested_type`, so every compiler configuration that builds Codegen already exercises self-recursive `std::vector<T>`. `codegen.recursive.map.runtime` is enabled only when a configure-time probe confirms that the active standard library accepts `std::map<K, Self>`; recursive maps have no equivalent standard guarantee.

The parser unit tests are in [`../tests/codegen/parser/test_parser.cpp`](../tests/codegen/parser/test_parser.cpp). The real-world differential corpus and comparison harness against `protoc` live under [`../tests/codegen/parser/differential/`](../tests/codegen/parser/differential/).

`codegen.imports.graph.exhaustive` is part of the normal suite. It deterministically enumerates every directed import graph with one through four files (66,066 graphs total) and compares `bind_import_edges()` against an independent transitive-closure reference. The test is intentionally default because it is in-memory and cheap.

Slower deterministic stress/property tests are available separately:

```sh
cmake -S . -B build-extended -DCMAKE_BUILD_TYPE=Release -DEASYPB_EXTENDED_TESTS=ON
cmake --build build-extended --config Release
ctest --test-dir build-extended -C Release -L extended --output-on-failure
```

The extended suite adds large deterministic random import graphs and `allow_missing` graph cases. These tests are not registered by default and CI runs them once per operating system (Linux, Windows, macOS) with the default toolchain rather than multiplying them across the compatibility matrix.

To verify the descriptor-set-only build separately:

```sh
cmake -S . -B build-lite -DCMAKE_BUILD_TYPE=Release -DEASYPB_CODEGEN_WITH_PROTO_PARSER=OFF
cmake --build build-lite --config Release
ctest --test-dir build-lite -C Release --output-on-failure
```

## CMake 3.10–3.19

The project build files remain compatible with CMake 3.10. The `cmake -S/-B` form was added in CMake 3.13, and `ctest --test-dir` was added in CMake 3.20. With an older CMake, configure and test from inside a fresh build directory instead:

```sh
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release
ctest -C Release --output-on-failure
```

For a descriptor-set-only build, use a separate `build-lite` directory and add `-DEASYPB_CODEGEN_WITH_PROTO_PARSER=OFF` to the configure command.
