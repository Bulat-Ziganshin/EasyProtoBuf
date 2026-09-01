# Runtime manual example

This directory contains the complete, compilable version of the hand-written two-way codec shown in the
[runtime manual](../../docs/manual.md#2-complete-example).

Files:

- `job.proto` — the reference Protobuf schema;
- `main.cpp` — matching C++ data types, Encoder and Decoder overloads, object initialization, and round-trip
  validation.

The example deliberately writes every field. Production codecs can add presence and default-value policies as
described by the runtime manual. The source is C++11-compatible.

Build and run from the repository root:

```sh
cmake -S . -B build/manual-example
cmake --build build/manual-example --target manual_example
./build/manual-example/manual_example
```
