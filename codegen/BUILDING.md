# Building and testing EasyProtoBuf Codegen

The modern command-line examples below require CMake 3.20 or newer. They set `CMAKE_BUILD_TYPE` for single-configuration generators and also pass `--config` / `-C` for multi-configuration generators such as Visual Studio, so both build styles consistently use `Release`.

## Building

From the repository root, build the project with CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The CMake target is named `easypb_codegen` internally and produces the `codegen` executable.

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

The [`codegen.modes`](../tests/codegen/parser/test_codegen_modes.cmake) test checks explicit and implicit descriptor-set input, `.proto` versus `.pbs` generated-code equivalence, proto2/proto3 packed behavior, descriptor printing, parser benchmarking, unresolved-type handling, and invalid empty/multi-file descriptor sets.

The [`codegen.maps`](../tests/codegen/maps/test_codegen_maps.cmake) test covers scalar, enum and message-valued map generation, custom map containers, malformed map descriptors, and `.proto`/`.pbs` equivalence. `codegen.maps.runtime` compiles the generated ADL codecs and verifies scalar, enum and message-valued map round trips in both full and descriptor-set-only builds.

The [`codegen.enums`](../tests/codegen/enums/test_codegen_enums.cmake) test covers top-level and nested enum declarations, fixed underlying types, qualified defaults, shadowing, aliases, negative values, and `.proto`/`.pbs` equivalence. `codegen.enums.runtime` verifies enum wire behavior, including preservation of unknown proto3 enum values.

The nested-message checks in [`codegen.modes`](../tests/codegen/parser/test_codegen_modes.cmake) cover lexical C++ nesting, qualified names, forward-reference ordering, `.proto`/`.pbs` equivalence, and clean rejection of recursive value dependencies. [`codegen.nested.runtime`](../tests/codegen/nested/nested_runtime.cpp) compiles the generated C++11 code and verifies nested-message and nested-message-map round trips.

The parser unit tests are in [`../tests/codegen/parser/test_parser.cpp`](../tests/codegen/parser/test_parser.cpp). The real-world differential corpus and comparison harness against `protoc` live under [`../tests/codegen/parser/differential/`](../tests/codegen/parser/differential/).

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
