# File-tree benchmark

This C++17 example benchmarks EasyProtoBuf on a large recursive data structure. It scans a directory into an in-memory tree, serializes the tree to the Protobuf wire format, deserializes it, validates the result, and reports the time and throughput of every stage.

## Files

- `filetree.proto` — the Protobuf schema.
- `filetree.pb.hpp` — plain C++ structures and inline EasyProtoBuf codecs.
- `main.cpp` — the name arena, filesystem scanner, progress display, timing, validation, and reporting.

The example uses `include/easypb.hpp` and has no other dependencies.

## Data model and wire format

`FileTree` contains one root `Node`. Every node stores:

- `name`: `string`, represented in C++ as `std::string_view`;
- `kind`: `fixed32`, identifying a regular file, directory, symbolic link, or other node;
- `size`: optional `fixed64`, present for regular files;
- `last_write_time_unix_ns`: optional `sfixed64`;
- `permissions`: optional `fixed32`, containing the portable `std::filesystem::perms` bit mask;
- `symlink_target`: optional `string`, represented as `std::string_view`;
- `children`: repeated `Node`, populated only for directories.

The wire representation is deliberately tuned for faster encoding and decoding. All numeric Protobuf fields use fixed-width wire types instead of varints, while names and symbolic-link targets are represented as `std::string_view`. Strings and nested messages remain length-delimited as required by Protobuf.

During scanning, the string views point into a monotonic arena. During decoding, they point directly into the serialized buffer. This avoids allocating and copying every decoded string; consequently, the serialized buffer must outlive the decoded tree.

## Name arena

The arena owns independent heap buffers. Buffer capacity starts at 4 KiB, doubles through 2 MiB, and remains 2 MiB for subsequent buffers. Each string is copied contiguously into one buffer and exposed as `std::string_view`. An oversized string receives a dedicated buffer large enough for that string.

## Filesystem scanning

The scanner uses `std::filesystem` and does not follow directory symbolic links. A symbolic link is recorded as a node, and its target is stored when readable.

Permission errors, metadata errors, invalid path conversions, and other recoverable filesystem failures increment the scan-error counter without terminating the benchmark. Failure while processing one entry skips that entry and continues with the next one. Failure while constructing or advancing a directory iterator abandons the remainder of that directory and lets its caller continue with the next sibling. Allocation failure remains fatal.

Directory traversal uses the iterator's native order because sorting would add unrelated benchmark work.

## Progress display

Progress runs synchronously in the scanning thread; no background thread is used. Whenever the combined regular-file and directory count is divisible by 256, the scanner checks the clock.

The indicator is exactly 99 terminal characters wide. Its fixed right-hand part contains the regular-file count, directory count, average total entries per second, and whole elapsed seconds at the end of the line. The current directory relative to the scan root uses all remaining width and is truncated as needed.

The first scheduled refresh is at one elapsed second. After each refresh, the reporter computes the absolute start-relative boundary of the next whole second. This avoids accumulating timer drift; the actual refresh follows that boundary at the next 256-entry clock check.

The complete line is assembled before one console write. The final progress line is erased before the report, so the report replaces it on the same terminal line.

## Benchmark stages

Three stages are timed independently with `std::chrono::steady_clock`:

1. Scan the directory into the first in-memory tree.
2. Encode the tree into a `std::string` buffer.
3. Decode a second tree from that buffer.

The report starts with the scanned root, logical file bytes, scan errors, and a compact breakdown of all entries. It then reports file-name and directory-name byte statistics, name-arena memory, serialized-buffer size, and the stage table.

For every stage, `MiB/s` is calculated from the serialized-buffer size. `Entries/s` uses the total node count, including the root directory.

Potentially large integral values and rates use apostrophes as decimal-group separators, for example `1'032'044`. Name averages, stage time in seconds, and MiB/s remain ungrouped.

## Building

From the EasyProtoBuf repository root:

### GCC

```sh
g++ -std=c++17 -O3 -Iinclude examples/filetree/main.cpp -o filetree
```

### Clang

```sh
clang++ -std=c++17 -O3 -Iinclude examples/filetree/main.cpp -o filetree
```

### Microsoft Visual C++

```bat
cl /std:c++17 /O2 /EHsc /Iinclude examples\filetree\main.cpp /Fe:filetree.exe
```

## Running

Pass exactly one directory to scan:

```sh
./filetree /path/to/directory
```

On Windows:

```bat
filetree.exe C:\
```

## Example output

The progress indicator is continuously replaced while the directory is scanned. When scanning finishes, the final indicator is erased and replaced by a report such as:

```text
Scanned C:/, found 929'830'619'018 bytes (886'755.580 MiB), scan errors: 11'247

                           Count       Total bytes     Average bytes
File names             1'032'930        32'715'083             31.67
Directory names          254'571         7'991'561             31.39
Other nodes                5'583           139'882             25.05
TOTAL                  1'293'084        40'846'526             31.59

Name arena:  used 40'846'526 bytes,  allocated 41'938'944 bytes = 28 buffers
Serialized buffer:  84'979'008 bytes (81.042 MiB)

Stage          Time (s)          MiB/s        Entries/s
Scan         164.200710           0.49            7'875
Encode         0.107815         751.68       11'993'589
Decode         0.185269         437.43        6'979'498

Validation: OK
```

The benchmark may take a long time on a large tree. Recoverable filesystem errors are counted in `Scan errors`; they do not make the run fail. Invalid command-line usage, an invalid scan root, allocation failure, decoding failure, or validation failure returns a nonzero exit status.
