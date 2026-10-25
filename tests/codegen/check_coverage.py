#!/usr/bin/env python3
"""Validate the codegen lane coverage map (read-only).

Run from anywhere:

    python tests/codegen/check_coverage.py

`tests/codegen/COVERAGE.md` is one table with one row per lane. The map is a
lane-level index, not a per-fixture ownership table: a fixture may prove
several properties, and the lane README plus the test names are the detail.

Checks:

  - the 16 fixed lane directories exist and `COVERAGE.md` has exactly one row
    per lane (the lane list is fixed, so a later non-lane directory such as
    `tests/codegen/harness/` is ignored);
  - every property P1..P9 and every exception E1..E14 appears in the table;
  - every CR-01..CR-48 appears in the table or in a source file under
    `tests/codegen/` (`.cmake`, `.cpp`, `.hpp`, `.py`, `.md`, `.tsv`); the
    table itself is not counted as the second source;
  - no per-fixture manifest from B13 (`cases.tsv`, `cases-ignore.tsv`,
    `check_cases.py`) remains anywhere under `tests/codegen/`.

It prints a compact `lane: properties; CR count` summary and exits nonzero
with the missing or unknown IDs listed.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
COVERAGE = os.path.join(HERE, "COVERAGE.md")

LANES = (
    "cli", "custom_types", "enums", "imports", "library", "limits", "maps",
    "names", "nested", "options", "packages", "parity", "parser", "recursive",
    "schema", "validity",
)
PROPERTIES = tuple("P%d" % i for i in range(1, 10))
EXCEPTIONS = tuple("E%d" % i for i in range(1, 15))
CRS = tuple("CR-%02d" % i for i in range(1, 49))
SOURCE_EXTENSIONS = {".cmake", ".cpp", ".hpp", ".py", ".md", ".tsv"}
RETIRED = {"cases.tsv", "cases-ignore.tsv", "check_cases.py"}

failures = []


def fail(message):
    failures.append(message)


def rel(path):
    return os.path.relpath(path, HERE).replace("\\", "/")


def read_table():
    """Return {lane: [cells]} from the first markdown table in COVERAGE.md."""
    if not os.path.isfile(COVERAGE):
        fail("COVERAGE.md is missing")
        return {}
    with open(COVERAGE, encoding="utf-8") as handle:
        lines = handle.read().splitlines()
    rows = {}
    header_seen = False
    for line in lines:
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if not header_seen:
            if cells[:2] == ["Lane", "What it proves"]:
                header_seen = True
                if len(cells) != 5:
                    fail("COVERAGE.md header has %d columns, expected 5"
                         % len(cells))
            continue
        if all(set(cell) <= set("-: ") for cell in cells):
            continue
        if len(cells) != 5:
            fail("COVERAGE.md row has %d columns, expected 5: %r"
                 % (len(cells), cells[0]))
            continue
        lane = cells[0].strip("`")
        if lane in rows:
            fail("COVERAGE.md has a second row for lane %r" % lane)
            continue
        rows[lane] = cells
    if not header_seen:
        fail("COVERAGE.md has no table header "
             "'| Lane | What it proves | ... |'")
    return rows


def ids_in(text, pattern):
    return set(re.findall(pattern, text))


def scan_sources():
    """CR IDs named by a source file under tests/codegen, table excluded."""
    found = set()
    for root, dirs, files in os.walk(HERE):
        dirs[:] = sorted(name for name in dirs
                         if name not in {".git", "__pycache__"})
        for name in files:
            path = os.path.join(root, name)
            if path == COVERAGE:
                continue
            if os.path.splitext(name)[1].lower() not in SOURCE_EXTENSIONS:
                continue
            try:
                with open(path, encoding="utf-8", errors="replace") as handle:
                    found.update(ids_in(handle.read(), r"CR-\d\d"))
            except OSError as error:
                fail("cannot read %s: %s" % (rel(path), error))
    return found


def find_retired():
    found = []
    for root, dirs, files in os.walk(HERE):
        dirs[:] = sorted(name for name in dirs
                         if name not in {".git", "__pycache__"})
        for name in files:
            if name in RETIRED:
                found.append(rel(os.path.join(root, name)))
    return found


def main():
    rows = read_table()

    for lane in LANES:
        if not os.path.isdir(os.path.join(HERE, lane)):
            fail("lane directory %r is missing" % lane)
        if lane not in rows:
            fail("COVERAGE.md has no row for lane %r" % lane)
    for lane in sorted(set(rows) - set(LANES)):
        fail("COVERAGE.md has a row for unknown lane %r" % lane)

    table_properties = set()
    table_exceptions = set()
    table_crs = set()
    summary = []
    for lane in LANES:
        cells = rows.get(lane)
        if not cells:
            continue
        properties = ids_in(cells[2], r"P\d+")
        for prop in properties - set(PROPERTIES):
            fail("%s: unknown property %r" % (lane, prop))
        exceptions = ids_in(cells[3], r"E\d+")
        for exception in exceptions - set(EXCEPTIONS):
            fail("%s: unknown exception %r" % (lane, exception))
        crs = ids_in(cells[3], r"CR-\d\d")
        for cr in crs - set(CRS):
            fail("%s: unknown CR %r" % (lane, cr))
        table_properties.update(properties & set(PROPERTIES))
        table_exceptions.update(exceptions & set(EXCEPTIONS))
        table_crs.update(crs & set(CRS))
        summary.append("%s: %s; CR %d"
                       % (lane, "; ".join(sorted(properties,
                                                 key=lambda p: int(p[1:]))),
                          len(crs)))

    source_crs = scan_sources()
    covered_crs = table_crs | source_crs

    missing_properties = [p for p in PROPERTIES if p not in table_properties]
    missing_exceptions = [e for e in EXCEPTIONS if e not in table_exceptions]
    missing_crs = [cr for cr in CRS if cr not in covered_crs]
    if missing_properties:
        fail("no row covers property/properties: %s"
             % ", ".join(missing_properties))
    if missing_exceptions:
        fail("no row covers exception(s): %s" % ", ".join(missing_exceptions))
    if missing_crs:
        fail("no row or source file names CR(s): %s" % ", ".join(missing_crs))

    retired = find_retired()
    if retired:
        fail("retired per-fixture manifest file(s) still present: %s"
             % ", ".join(sorted(retired)))

    if failures:
        print("FAIL: %d problem(s)" % len(failures))
        for item in failures:
            print("  -", item)
        return 1

    print("PASS: %d lanes, properties %d/%d, exceptions %d/%d, CRs %d/%d "
          "(table %d, tests %d)"
          % (len(LANES), len(table_properties), len(PROPERTIES),
             len(table_exceptions), len(EXCEPTIONS), len(covered_crs),
             len(CRS), len(table_crs), len(source_crs)))
    for line in summary:
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
