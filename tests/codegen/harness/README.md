# Differential harness — B2/M1, M2 discovery and B2b quality

This is the tracked copy of the **M1 pilot**, the **M2 discovery slice** and
the **B2b quality half**: a runner, a seed corpus, the P1/P2 comparator, a
seeded grammar generator, a descriptor-set mutator with a hand-built wire
writer, a P3 compile matrix, a P7 thin/full check and a delta-debugging
shrinker.

## Tracked layout and CI

- `corpus/rows/` holds one sidecar per row; `corpus/generated/` holds the
  minimized inputs of the discovery and hand-built regressions. Both are
  tracked. `corpus/review/` holds the review fixtures the seed corpus names,
  copied here so the harness is self-contained in a clean checkout.
- `runs/`, `generated/`, `__pycache__/` and the summary JSON files are run
  output. They are not tracked; `.gitignore` keeps them out of a local status.
- CI copies this directory to `build/codegen-harness/` and runs it there, so a
  run never dirties the checkout. The repository root is found by walking up
  to the directory that owns `CMakeLists.txt` and `tests/codegen`, which works
  from both `tests/codegen/harness/` and `build/codegen-harness/`.
- CI always passes `--protoc`, `--codegen`, `--lite-codegen` and `--gxx`. The
  same values can be supplied through `EASYPB_PROTOC`, `EASYPB_CODEGEN` and
  `EASYPB_GXX` for a local shell; a bare name is resolved through `PATH`.
- `protoc.lock` pins the oracle release (`30.1`), its Linux x86-64 archive URL
  and SHA-256. The workflow downloads that archive, verifies the hash and
  passes the same binary to CMake and the harness.

What M1 contains:

- one isolated directory per case, both tools invoked with an argv list (no
  shell), a timeout, a fixed environment, and a capture of rc/stdout/stderr and
  every generated file;
- the 48 review rows from the local review ledger
  `build/code-review-1023-ds/repro-index.md` (the fixtures it names are copied
  to `corpus/review/`) and the rows built from the existing `tests/codegen/`
  fixtures;
- P1 verdict parity with `protoc` 30.1 on the source and descriptor paths;
- P2 SHA-256 identity between the header generated from source and the header
  generated from the protoc-written `FileDescriptorSet`;
- `summary.json` with cases, mismatches, classifications, crashes, timeouts and
  the B1 denominators.

What the M2 discovery slice adds:

- `generate.py` — a seeded, deterministic grammar generator for valid `.proto`
  trees plus one-rule-at-a-time source mutations;
- `mutate.py` — a structural mutator over a protoc-written
  `FileDescriptorSet`, recording the descriptor field it touched (the C1
  coverage counter);
- `discover.py` — 200 generated cases (fixed plan per seed), run through the
  same runner and P1/P2 comparator, classified as `pass`, `known root`,
  `allowed-stricter`, `new root` or `false positive`;
- new instances of R1–R10 copied into `corpus/generated/<id>/` with a corpus
  sidecar; a new root is written to `pilot-findings.md` and the run stops.

What the B2b quality half adds:

- `compile.py` — the P3 compile matrix: every accepted header under
  `-std=c++11/-std=c++17 -pedantic-errors`, plus the lite (parser-free) profile
  on the descriptor rows;
- `thin.py` — the P7 check: thin/full/source header SHA-256 and the
  placement-new external-enum probe from CR-08;
- `shrink.py` — delta debugging on `.proto` text and descriptor bytes; it
  minimized all 77 raw generated rows and wrote the minimized inputs back into
  the corpus;
- `wire.py` + `handbuilt.py` — a wire writer and a fixed plan of 14 descriptor
  sets that `protoc` would never write (a duplicate field number, an absent
  kind, an editions file, a repeated public index, …);
- the ten `inverse` rows of `parity-cases.tsv` are inside the corpus with their
  P1 exception IDs (E8/E9/E10/E4), so no row is outside the baseline.

## One command

M1 seed corpus:

```powershell
python tests/codegen/harness/run.py --cases 200 --seed 1
```

M2 discovery slice (generator + mutator, 200 cases, fixed seed):

```powershell
python tests/codegen/harness/discover.py --cases 200 --seed 1
```

`run.py` writes `runs/<run-id>/` and refreshes `summary.json` in this directory.
`discover.py` writes `generated/s<seed>/` (inputs and manifest), `runs/m2-<run-id>/`
(per-case results) and `discovery-summary.json` (the latest discovery summary).
Both commands are deterministic: with the same corpus, tools and seed they
produce the same inputs and the same per-case classifications. The M2 run was
verified on 2026-10-24 by two full runs (`classifications identical: True` for
all 200 cases) and by `generate.py --seed 1 --check`.

The step's evidence commands also include the unit checks:

```sh
python tests/codegen/harness/selftest.py
python tests/codegen/harness/seed.py --check
python tests/codegen/harness/generate.py --seed 1 --check
python tests/codegen/harness/mutate.py
python tests/codegen/harness/handbuilt.py
python tests/codegen/harness/thin.py
python tests/codegen/harness/compile.py --cases 0 --jobs 8 \
    --lite-codegen build/b2b-probe/tree-lite/codegen.exe
python tests/codegen/harness/shrink.py --all
```

`compile.py` needs a parser-free codegen for the lite profile
(`cmake -DEASYPB_CODEGEN_WITH_PROTO_PARSER=OFF`); without `--lite-codegen` the
lite profile is recorded as not measured. `shrink.py` is convergent: a pass
on already-minimized rows only tries the remaining reductions, and the third
pass was a fixed point.

Useful options:

| Option | Meaning |
| --- | --- |
| `--cases 0` | run every comparable row |
| `--only TEXT` | run rows whose id contains `TEXT`; repeatable |
| `--protoc PATH` | oracle binary; defaults to `$EASYPB_PROTOC` or `protoc` on `PATH` |
| `--codegen PATH` | product binary; defaults to `$EASYPB_CODEGEN` or `codegen` on `PATH` |
| `--out DIR` | write the run under a fixed directory |
| `--quiet` | no per-case progress line |

M1's `--cases 200` originally ran all 195 comparable rows. The corpus now also
contains the 77 minimized generated regression rows and the 11 hand-built
regression rows, so `--cases 200` samples 200 of 293 comparable rows; pass
`--cases 0` to run everything. `discover.py` always runs its fixed 200-case
plan and never samples.


## Corpus

`seed.py` is the reproducible description of the corpus. It writes one sidecar
per row:

```
corpus/rows/<id>.expect.json
```

A sidecar is the unit of the corpus; there is no separate manifest. Fields:

| Field | Meaning |
| --- | --- |
| `id` | `CR-nn` for review rows, `t-...` for test-derived rows, `m2-...` for generated rows, `hb-...` for hand-built rows |
| `source` | `review`, `test`, `generated` or `handbuilt` |
| `kind` | `source`, `descriptor`, `not-comparable` |
| `properties` | properties the row is meant to exercise |
| `input` | repository-relative fixture (the runner never writes to it) |
| `minimized` | B2b shrinker record: verified, sizes, evaluations, raw-kept flag |
| `include_roots` | `-I` roots for the source path |
| `target` | logical name for `--descriptor-set` (from the `.pbs` file name) |
| `cr`, `root`, `exception` | review ledger mapping; `root: null` for test rows; generated rows carry the root they reproduced |
| `p1_expect`, `p2_expect` | the measured verdict (documentation) |
| `timeout_s` | per-process timeout; 60 s for the stress row |
| `mandatory` | review rows and generated regressions are never sampled out |
| `reason` | why a `not-comparable` row is outside M1 |

Current composition (`python tests/codegen/harness/seed.py`):

| Group | Rows |
| --- | --- |
| review rows, comparable | 31 |
| review rows, not comparable | 17 |
| test-derived rows (parity lanes, accepted controls, B12 families) | 221 |
| M2 discovery regressions (`source: generated`) | 77 |
| B2b hand-built descriptor sets (`source: handbuilt`) | 6 |
| **total / comparable** | **352 / 335** |

The 107 import-parity rows are the 97 non-inverse rows plus the ten `inverse`
rows that B2b classified with an exception ID. The four parity shapes with
`expected: DIAGNOSTIC_*` stay outside as `not-comparable` only when their set
has no target or no comparable verdict; no verifiable row is left out for lack
of an exception.

### Curation rules

- Only rows whose lane contract is `protoc` parity are included. The
  `parity-cases.tsv` rows carry a declared protoc verdict; the ten `inverse`
  rows are included with the P1 exception ID that records the owner decision
  (E8 for A25 editions, E9 for A18 import indexes, E10 for A26 map entries,
  E4 for W-10 field kinds), so they classify as `exception`, not as an
  unexplained divergence.
- The `tests/codegen/parity` lane is included in both directions where a
  `.proto` source exists; the `w36-*` pairs are descriptor-only.
- Accepted controls are the fixtures whose README says the reference compiler
  accepts them. Refusal fixtures for the documented stricter rules (E1–E4) are
  not mixed into the parity baseline.
- The 17 review rows that M1 cannot compare are still recorded, with a reason.
  They are CLI/IO, print-mode, compiler, or compatibility-mode cases whose
  oracle is P3/P4/P5/P7/P9, not P1/P2. B2b maps each to its owning fix step
  (B5, B6, B8, B9, B10 or B11) in `steps/B2b.md`.
- Generated rows are added only for a new instance of a known root. Passes,
  allowed-stricter cases and new roots are not corpus rows: a new root stops
  the run for an owner decision, and a policy case is already covered by the
  diagnostic-keyed policy table.
- Hand-built rows are added by `handbuilt.py` for the shapes `protoc` refuses
  to write; each carries the root or exception its owner decision assigns.

## M2 discovery inputs

`discover.py --cases 200 --seed 1` runs one fixed plan; every case is built
from the seed by a SplitMix64 stream, so the input bytes are reproducible:

| Mode | Cases | Families |
| --- | --- | --- |
| `valid` | 120 | scalars2 (18), scalars3 (12), defaults (22), names (10), policy-names (4), enums (10), imports (16), services (10), maps-oneof (12), limits (6) |
| `source-mutation` | 62 | lexer/literal R2 probes (26), source grammar R5 probes (33), E5 keyword packages (3) |
| `descriptor-mutation` | 18 | one operation per case from `mutate.py` |

The valid families cover `proto2`/`proto3`, every scalar type, labels,
defaults with octal/hex/inf/nan/escape spellings, nested messages and enums,
maps, oneofs, services, imports (plain/public/weak/diamond), packages, field
number boundaries, reserved ranges and long/wide declarations. The source
mutations break one lexical or grammar rule at a time. The descriptor mutator
starts from a protoc-written set and rewrites one field; the operation list and
the descriptor path it touched are in `mutate.OPERATIONS`.

Deliberate exclusions in the discovery plan:

- proto3 `optional` / synthetic oneofs (B1 gap G1) and known-option legality
  (G2) are not generated until their own step decides the baseline;
- R7–R10 are CLI/print/resource properties and are not observable in P1/P2, so
  the generator does not fake them;
- the discovery plan is one profile and no compile step; P3/P7 and the shrinker
  are separate B2b commands (`compile.py`, `thin.py`, `shrink.py`).

## Hand-built descriptor sets

`wire.py` writes `FileDescriptorSet` bytes directly; `mutate.py`'s
`HAND_BUILT` table builds fourteen shapes that `protoc` refuses to produce
(a duplicate field number, an absent or unknown field kind, a proto2 group, a
proto3 required field, `message_set_wire_format` with fields, a reserved-range
overlap, an extension range above the protocol maximum, an unreferenced
malformed map entry, a repeated public index, an editions file, a method
endpoint naming a map entry, a bracket-shaped `json_name`). Each shape carries
the class the owner decision predicts:

- `parity` — both tools refuse (duplicate number, field number zero, proto3
  required), or both accept after a root was closed (the three B5 shapes and,
  since B8, the map-entry method endpoint and the bracket-shaped `json_name`);
- `exception` — codegen refuses, protoc accepts, and the refusal carries the
  E4/E8/E9/E10 marker;
- `known-root` — the tools diverge in the direction the triage root describes.
  No shape is in this class any more: B8 moved its last two shapes (`R6`) to
  `parity`.

`handbuilt.py` writes the six non-parity shapes into the corpus
(`source: handbuilt`) and is a fixed, reproducible plan. A shape that changes
class is a new finding and exits 1.

## P3 compile matrix

`compile.py` runs the corpus through the normal runner, then, for every row the
product accepts:

1. generates the target header with the row's own invocation;
2. generates every file of the descriptor closure into a sibling directory, so
   the dependency headers the header contract asks the caller to generate are
   present;
3. compiles a one-line TU that includes the target header under
   `-std=c++11 -pedantic-errors -fsyntax-only` and the same for C++17
   (parallel, one fresh case directory per row);
4. with `--lite-codegen`, generates the descriptor rows with the parser-free
   binary, compares the header SHA-256 with the full build's, and compiles the
   lite header when it differs.

A generated header is `#pragma once` plus inline, non-template definitions, so
the include is a complete semantic check of the declarations and the codec
bodies. A nonzero compile is a P3 verdict. Failures are classified against the
catalogue's P3 status: a row listed there is `known-p3-row`; a generated row
whose diagnostic matches the root's catalogue signature (for example the CR-05
`integer constant is so large` line on an R1 row) is
`known-root-instance:CR-nn`; everything else is `unexplained` and exits 1.

`p3-summary.json` records `cases_accepted`, `cases_measured`, the per-profile
matrix, `known_baseline`, `unexplained` and the lite identity count. The pinned
`bbbd000` probe is a **pre-fix baseline**; B17 re-runs the matrix after the fix
steps.

## P7 thin vs full

`thin.py` generates the same target from three inputs — the CR-08 `thin.pbs`
(compatibility path, no `--target`), `full.pbs` (`--target b.proto`) and the
`ext/` source tree — and compares the three SHA-256 values. It then compiles
`thin-read.cpp` against the thin and the full header and reads the
placement-new external-enum value.

At the pinned `bbbd000` probe `full == source`, the thin header differs, and
the thin probe reads `e = -1414812757` while the full probe reads `e = 0`. That
is exactly the CR-08 signature and is recorded as `known-cr-08`; any other
divergence is `unexplained` and exits 1. B17 re-runs the check after B8.

## Shrinker

`shrink.py` implements delta debugging over a list of units:

- `.proto` — brace-balanced top-level declarations (`syntax`, `package`,
  `import`, `option`, `message`, `enum`, `service`), then recursively the body
  of every declaration, so an unused field, option, import or message can
  disappear;
- `FileDescriptorSet` — the wire fields of the set, then recursively the known
  message-typed fields (file, message, field, enum, service, method), then a
  leaf pass that tries the shortest string and a zero varint.

The verdict is pinned, not just "did something diverge": P1 classification,
both accept bits, the P2 applicability/match pair, the normalized first
diagnostic line of each tool, the P2 first differing value and any crash/timeout
flags. A candidate that changes the mechanism is rejected.

Every generated row gets a `minimized` record in its sidecar (`verified`,
`size_before`, `size_after`, `evaluations`, `duration_s`, `raw_kept`). A
minimized input replaces the raw one in `corpus/generated/<id>/`; the raw bytes
are kept under `raw/` only when the minimized input reproduces a different
verdict (which the shrinker never accepts). `discover.py` refuses to overwrite
a minimized row on a later run.

B2b's first pass minimized all 77 rows: 31 800 → 5 038 bytes, 77/77 verdicts
verified. A second pass shaved the last 19 bytes to 5 019; a third pass was a
fixed point (5 019 → 5 019).

## M2 discovery classification

`discover.py` reads the captured runner results only. A case is exactly one of:

| Classification | Rule |
| --- | --- |
| `pass` | P1 agrees and P2 is not applicable or agrees |
| `known root` | the divergence is the mechanism of R1–R6 that the case was built to probe |
| `allowed-stricter` | codegen refuses, protoc accepts, and the diagnostic is a documented P1 policy or an E1–E6 exception (E4/E5 are row-level here) |
| `new root` | no explanation; written to `pilot-findings.md`, run stops |
| `false positive` | a comparator invariant fails (including "both tools could not read the input") |
| `generator-error` | a valid-by-construction case is rejected by protoc; not a finding |

Known-root mapping: P2-only with a default literal → R1; `r2-*` → R2; `d-*`
with an R3/R6 operation → that root; `r5-*` → R5; a one-sided crash/timeout is
never folded into a family root — it is a new root. The default-value root is
restricted to a P2-only divergence, and the first differing line is recorded as
evidence.

## P1 — acceptance parity

For every case both tools run on the same input:

- source path: `codegen -I <roots> --out-dir ... <file.proto>` and
  `protoc -I <roots> --cpp_out=... --descriptor_set_out=... --include_imports <file.proto>`;
- descriptor path: `codegen --descriptor-set --target <logical> --out-dir ... <file.pbs>`
  and `protoc --descriptor_set_in=<file.pbs> --cpp_out=... <logical>`.

"Accepts" means exit code 0, no timeout, and a non-empty generated artefact
(`.pb.hpp` for codegen, `.pb.h` or `.pb.cc` for protoc). A crash is a refusal,
not a runner error; it is reported separately.

Every P1 difference is classified as one of:

| Classification | Meaning |
| --- | --- |
| `pass` | the verdicts agree |
| `known` | the row names a root from `triage.csv` |
| `exception` | the row names an exception from `property-catalogue.md` (currently E6) |
| `allowed-stricter` | codegen refuses, protoc accepts, and the P1 policy allows it because the header would be invalid C++ |
| `false-positive` | the row is marked `false_positives: ["P1"]`; a comparator bug |
| `new` | no explanation; a real counterexample for the triage |

The `allowed-stricter` policy table is in `compare.py::ALLOWED_STRICTER`:

| Marker in codegen stderr | Authority |
| --- | --- |
| `recursive message value dependency` | `codegen/GENERATED_CODE.md`: message fields are C++ values; a by-value recursive graph has no finite type |
| `would both be named` | `tests/codegen/names/README.md`, PC-02: the escape rule is not injective, so refusing avoids two identical C++ names |
| `collides with the presence flag` | `codegen/GENERATED_CODE.md`: a presence flag that collides with a member of its own message would not compile; the reference compiler accepts the shape |

The policy is checked against the diagnostic, not against a fixture name, so a
new input that triggers the same documented rule classifies the same way.

## P2 — cross-path identity

For a source case that both tools accept, protoc also writes a
`FileDescriptorSet` (`--include_imports`), and codegen generates the same logical
file from that set with `--target`. The comparator compares the SHA-256 of the
two headers and records the first differing line when they differ.

P2 is not applicable when the source or descriptor path refuses, or when the
case is a descriptor-only row. A descriptor path that refuses after a successful
source path is a P2 mismatch, not "not applicable".

P2 has no exception list: with the current product the only two mismatches are
CR-01 and CR-17, both root R1 and both recorded as `known`.

## Runner contract

`runner.py`:

- creates a fresh case directory under `runs/<run-id>/<case-id>/`;
- runs `subprocess.run(argv, shell=False)` from that directory;
- sets `LC_ALL=C`, `LANG=C`, `TZ=UTC` and a per-case `TMP`/`TEMP`/`TMPDIR`;
- kills the process on timeout and records `timed_out`;
- captures stdout/stderr as bytes (`*.stdout.bin`, `*.stderr.bin`) and a file
  tree with relative path, size and SHA-256;
- recognises the Windows crash codes (`0xC00000FD`, `0xC0000005`, …).

No write goes to the repository: inputs are opened read-only, outputs go to the
case directory.

## Output

```
runs/<run-id>/
  <case-id>/
    result.json              # one case verdict, with argv and captured streams
    codegen/ protoc/ ...     # generated headers and artefacts
    *.stdout.bin *.stderr.bin
  results.json               # all case results
  summary.json               # the headline metrics
summary.json                 # copy of the latest run
```

`summary.json` keys: `cases_run`, `verdict_mismatches`, `p2_mismatches`,
`known_instances`, `allowed_stricter`, `new_roots`, `false_positives`,
`crashes`, `timeouts`, `runner_errors`, `duration_s`, `properties.P1/P2`,
`coverage` (B1 denominators from `check-inventory.py --json`) and
`not_comparable`.

The exit code is 0 when every difference is classified (pass/known/exception/
allowed-stricter) and there are no runner errors; it is 1 when a `new` root, a
false positive or a runner error appeared. This makes the command usable as a
non-blocking nightly gate later.

## M1 result (2026-10-24, before the generated rows)

`python tests/codegen/harness/run.py --cases 200 --seed 1`:

| Metric | Value |
| --- | --- |
| cases run / available | 195 / 195 |
| not comparable | 17 review rows (recorded with reasons) |
| P1 checks / mismatches | 195 / 24 |
| P2 applicable / mismatches | 50 / 2 |
| known instances | 19 (17 P1 instances + CR-01/CR-17 P2) |
| allowed stricter | 7 |
| new roots | 0 |
| false positives | 0 |
| crashes | 1 case (CR-35: codegen and protoc both overflow the stack) |
| timeouts / runner errors | 0 / 0 |
| duration | 36 s |

The seed corpus is green in the M1 sense: every observed difference is
explained by the triage, an exception, an allowed-stricter policy or a known
shared hole. The comparator also reproduces the two expected P2 byte
divergences with their first differing line (`CR-01`: `10` vs `8`; `CR-17`:
`0x10` vs `16`).

After the generated rows were added, a rerun with
`python tests/codegen/harness/run.py --cases 200 --seed 1` selects 108
mandatory + 92 sampled rows of 272 comparable rows and reports 96 known
instances (77 generated + 19 seed), 5 allowed-stricter, 0 new roots, 0 false
positives and the CR-35 shared crash, in 30 s. The M1 table above is the
historical 195-row baseline, not the current sample composition.

The current full corpus (`--cases 0`, 293 comparable rows after the B2b
additions) reports 117 known instances (77 minimized generated, 11 hand-built,
10 inverse exceptions, 19 seed), 7 allowed-stricter, 0 new roots, 0 false
positives and the CR-35 shared crash, in 37.5 s.

## B2b quality result (2026-10-24/25)

All commands ran against the pinned probe
`build/code-review-1023-ds/tree-cxx17/codegen.exe`, SHA-256
`4160E2C85E0597820D4822457FC5874862941EF492A0F3C8EB32DC873DE5BAA9`.

| Check | Result |
| --- | --- |
| `selftest.py` | 27 tests OK (wire writer, hand-built plan, inverse rows, P3 classification, shrink units) |
| `run.py --cases 0` | 293 cases, 117 known, 7 allowed-stricter, 0 new roots, 0 false positives, 1 shared crash (CR-35), 37.5 s |
| `discover.py --cases 200 --seed 1` | 200 cases, 77 known (R1 22, R2 18, R3 3, R5 31, R6 3), 8 allowed-stricter, 0 new roots, 0 false positives, 15.6 s |
| `handbuilt.py` | 14/14 shapes match the predicted class; 11 corpus rows |
| `compile.py --cases 0` | 163 accepted headers; 151 pass both standards, 12 known catalogue measurements (CR-02/03/04/05/09/10 × 2 profiles), 0 unexplained; lite: 58/58 headers byte-identical, 0 failures; 52 s |
| `thin.py` | `full == source`; thin differs; thin probe `e = -1414812757`, full probe `e = 0` → `known-cr-08` |
| `shrink.py --all` | 77/77 verdicts verified; 31 800 → 5 019 bytes after three passes; 79 s first pass |
| false-positive rate | 0 / 293 seed and 0 / 200 generated (below 10%) |

The P3 and P7 red cells are the **pre-fix baseline** the step asks for: P3's
twelve failures are exactly the catalogue's P3 rows, and P7's divergence is
exactly CR-08 (R7, fixed by B8). B17 re-runs both after B3–B11. No failure is
unexplained.

## B8 re-run (2026-10-25, R6/R7)

The instruments above are re-run against the fix tree
(`build/verification/tree-cxx17/codegen.exe`, SHA-256
`29BCA709E53EB5E1E022AE330303F13B0AFD3555581E746398FB0339B910A870`
before B8) after the cross-path and thin-mode fixes:

| Check | Result |
| --- | --- |
| `thin.py --codegen build/verification/tree-cxx17/codegen.exe` | `verdict: pass` — thin, full and source share one SHA-256; both probes read `e = 0` |
| `handbuilt.py --codegen …` | 14/14 shapes match; `hb-method-map-entry-endpoint` and `hb-json-name-brackets` moved `known-root/R6` → `parity`, so the hand-built corpus is six rows |
| `run.py --cases 0 --codegen …` | 328 comparable rows, 25 known instances, 7 allowed-stricter, 0 new roots, 0 false positives, the CR-35 shared crash |
| `seed.py --check` | corpus rows match (351 after the six new parity fixtures) |

The two former `known-root` rows were deleted from `corpus/rows/` with their
`corpus/generated/<id>/` inputs: a parity shape is not a corpus row, exactly as
the B5 closure of R3 left only its five exception rows.

One R6-classified row remains: `m2-192-descriptor`, a duplicate of one logical
file *inside one container*. It is not CR-24's cross-container shape - the
W-36 owner decision of 2026-10-22 deliberately keeps an in-container duplicate
refused, which the property catalogue now records as exception `E12`. Its
`codegen` half is `test_loader_rejects_duplicate_names_within_one_container`.

## B11 re-run (2026-10-25, R9/R10)

`python tests/codegen/harness/run.py --cases 0 --codegen
build/verification/tree-cxx17/codegen.exe` after B11:

| Check | Result |
| --- | --- |
| cases | 334 comparable rows, 17 not comparable |
| P1 / P2 mismatches | 33 / 0 |
| known instances | 26 (the 25 of the B8 run plus CR-35) |
| allowed stricter | 7 |
| new roots / false positives | 0 / 0 |
| crashes / timeouts | 1 / 0 — the `protoc` half of CR-35 only |

CR-35 moved from "both tools overflow the stack" to "the product accepts the
12 000-message chain and protoc 30.1 still crashes on it". The row stays in
the corpus as a known R9 instance because a crash has no comparable verdict;
the P3/P8 claim ("no crash on an accepted input") is executable in the tracked
lane as `codegen.limits` → `dependency-chain`.

## M2 discovery result (2026-10-24)

`python tests/codegen/harness/discover.py --cases 200 --seed 1`:

| Metric | Value |
| --- | --- |
| cases run / requested | 200 / 200 (120 valid, 62 source mutations, 18 descriptor mutations) |
| pass | 115 |
| P1 / P2 mismatch cases | 63 / 22 (85 distinct cases) |
| known instances | 77 — R1 22, R2 18, R3 3, R5 31, R6 3 |
| allowed stricter | 8 — `non-injective-name-escape` 2, `presence-flag-collision` 2, E5 3, E4 1 |
| new roots | 0 |
| false positives | 0 |
| generator / runner errors | 0 / 0 |
| crashes / timeouts | 0 / 0 |
| corpus rows added | 77 (`corpus/generated/<id>/`, `source: generated`, mandatory) |
| duration | 15.3 s |
| coverage | 84 generator tags, 16 descriptor fields, 8 option names (B1 denominators reported separately) |

The classification is stable: a second full run produced an identical
`case_results` vector for all 200 cases, and `generate.py --seed 1 --check`
matched the stored manifest digest. The 77 generated rows also run through
`run.py --cases 0 --only m2-`: 77 known instances, 0 new roots, 0 false
positives.

This is a **discovery** result, not the B2 gate. The gate's "at least three real
counterexamples not already in the triage" is not met: new roots = 0. The
known instances are real counterexamples but they are already explained by
R1/R2/R3/R5/R6, so the corpus grew and the triage did not reopen.

`discovery-summary.json` keys: `seed`, `cases`, `new_roots`, `known_instances`,
`known_by_root`, `verdict_mismatches`, `p1_mismatches`, `p2_mismatches`,
`allowed_stricter`, `false_positives`, `generator_errors`, `runner_errors`,
`compile_failures` (`null`; P3 is a separate `compile.py` command), `crashes`,
`timeouts`, `coverage`, `inventory`, `corpus_rows`, `case_results`. The headline
counts are numeric; the matching `*_detail` lists name the cases and (for known
instances) the root and first difference.

The B2b commands each write one summary next to the harness and one under
`runs/<run-id>/`: `p3-summary.json` (compile matrix, per-row and per-profile
verdicts, lite identity), `p7-summary.json` (three hashes, probe values,
verdict), `shrink-summary.json` (per-row sizes, evaluations, verdict equality),
`handbuilt-summary.json` (shape, expected class, observed class) and
`discovery-summary.json`.

## B17 re-run (2026-10-25, stop review)

The instruments were re-run after all fixes on
`build/verification/tree-cxx17/codegen.exe` (SHA-256
`1A00EA883C51DCFDCFFB26BCF1388656E8A25F6FFA50FC2CBC33C49B823B94F2`):

| Check | Result |
| --- | --- |
| `run.py --cases 0` | 335 comparable rows; P1 33 mismatches (22 known/accepted + 11 allowed-stricter), P2 105/105, 0 new roots, 0 false positives, the CR-35 oracle crash |
| `discover.py --cases 200 --seed 1` | 200 cases, 191 pass, 9 allowed-stricter (E4 1, E5 3, E12 1, name rules 4), 0 known/new/false-positive/generator/runner errors |
| `discover.py --cases 10000 --seed 1` | 10 000 cases, 9 550 pass, 450 allowed-stricter, 0 new roots, 0 false positives, 0 generator/runner errors, 1 061.7 s |
| `discover.py --cases 10000 --seed 2` | 10 000 cases, 9 550 pass, 450 allowed-stricter, 0 new roots, 0 false positives, 0 generator/runner errors, 1 111.0 s |
| `compile.py --cases 0` | 166 accepted / 165 measured, 0 failures, 0 unexplained; lite 58/58 identical, 0 failures; `p3-postfix.json` |
| `thin.py` | `verdict: pass`; `p7-postfix.json` |

The 10 000-case form scales the fixed 200-case plan to consecutive rounds
(50 rounds); the round number is mixed into the per-case RNG stream, so the
rounds are different inputs with the same family balance. The 200-case output is
unchanged. `generate.py --seed <n> --cases 10000 --check` matches both stored
manifests. The pre-fix P7 `known-cr-08` baseline remains in
`runs/p7-20261024-235101/`; the B2b P3 baseline is in
`runs/p3-20261025-000245/`.

## Limitations

- `build/` is gitignored and machine-specific; the paths above are this
  machine's. On another checkout, pass `--protoc` and `--codegen`.
- The corpus is curated, not exhaustive. The CLI properties (P4/P5/P8/P9) are
  outside this harness; the 17 `not-comparable` rows are the visible boundary
  and each has an owning fix step (B5, B6, B8, B9, B10, B11).
- `protoc` is an oracle for parity, not truth. Its own crash on CR-35 is the
  remaining one-sided crash after B11 (the product accepts the chain); the row
  is kept as a known instance because a crash has no comparable verdict.
- The generated cases cover the mechanisms the generator knows (R1–R6 and
  E1–E6). R7–R10 need their own oracles; proto3 `optional` (G1) and known-option
  legality (G2) are not generated, but B12 closed both with tracked family
  tests, so that is a generator-coverage gap, not a product gap.
- P3 and P7 have a pre-fix baseline and the B17 post-fix verdict; use
  `p3-postfix.json` and `p7-postfix.json` for the current claim, not the
  historical red cells.
- The lite profile requires a parser-free codegen binary; without
  `--lite-codegen` the matrix records the profile as not measured rather than
  claiming a result.
- M1's `summary.json` is the latest seed-corpus run (the current corpus includes
  the minimized generated rows and the hand-built rows); the M1 table above is
  the historical baseline. The discovery result is in
  `discovery-summary.json`. Do not compare the two `known_instances` numbers
  directly: one counts seed rows, the other generated cases.
