# `tests/codegen` — lane layout and coverage map

Each lane directory holds one family of codegen tests: its fixtures under
`data/`, the driver or test source that runs them, and a `README.md` that
explains what the fixtures prove. The lanes run as the `codegen.*` CTest
family.

```text
tests/codegen/
  COVERAGE.md         one row per lane: purpose, properties, CRs/exceptions
  check_coverage.py   the coverage gate (pure Python, no protoc, no network)
  README.md           this page
  common/             shared `.pbs` fixture builder for five lanes
  <lane>/             fixtures, driver, README; no per-fixture manifest
```

## The coverage map

`COVERAGE.md` is a **lane-level** index, not a per-fixture ownership table:

```text
Lane | What it proves | Properties | CRs / exceptions | Notes
```

One fixture may prove several properties, and one test may cover several CRs;
the row names the IDs the lane's tests carry, while the lane `README.md` and
the test names remain the detailed explanation. The 16 lane names are fixed;
the differential harness in `tests/codegen/harness/` gets no lane row and is
excluded from the CR scan because it names CRs in its own code and README.

The map must name every property `P1`–`P9`, every exception `E1`–`E14` and
every CR `CR-01`–`CR-48` (the CR may also be named by a test source). The
property and exception definitions live in
`build/verification/property-catalogue.md`; the CR ledger is
`build/verification/triage.csv`.

## The gate

```powershell
python tests/codegen/check_coverage.py
```

The validator checks that the 16 lane directories exist, that `COVERAGE.md`
has exactly one row per lane and no unknown lane, that P1–P9 and E1–E14 are
listed, that every CR-01–CR-48 is named by the table or a source file under
`tests/codegen/` (the harness directory is excluded), and that the retired
per-fixture manifest (`cases.tsv`, `cases-ignore.tsv`, `check_cases.py`) is
gone. It prints `lane: properties; CR count` per lane and exits nonzero with
the missing IDs listed. CI runs it in the `coverage` job of
`.github/workflows/cmake-multi-platform.yml`; B17 runs it locally, and the
`extended-harness` workflow runs the differential harness itself.

## Adding a test

1. Put the fixture in the lane's `data/` directory and rebuild the `.pbs`
   with the lane's `make_descriptors.py` when one exists; never hand-edit a
   binary fixture. The five descriptor lanes share their encoder in
   `common/descriptor_fixtures.py`, and `--check` proves that the committed
   `.pbs` files are exactly what the generator produces.
2. Follow the lane `README.md`: most lanes require both input paths, a
   compiled header and a refusal transcript.
3. If the test adds a property, exception or CR the lane did not carry, add
   the ID to the lane's `COVERAGE.md` row and run the gate. A new exception
   needs the E-ID and an executable xfail first (see the property catalogue).
