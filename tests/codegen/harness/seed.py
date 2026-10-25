"""Build the M1 seed corpus for the differential harness.

The corpus is a directory of sidecars; each row is one JSON file under
``corpus/rows`` and names its input, the include roots and the expectation.
This script describes the seed and folds in the regression rows written by the
M2 discovery driver (``source: generated``); it does not generate cases,
mutate descriptors or shrink anything.

Sources of rows:

* the 48 review reproductions from ``build/code-review-1023-ds/repro-index.md``
  (``source: review``, mandatory);
* the declared protoc verdicts of the existing imports parity lane
  (``tests/codegen/imports/data/parity/parity-cases.tsv``);
* the existing cross-path parity lane, the differential corpus, the parser lane
  and a curated set of accepted controls from the validity/names/maps/enums/
  packages/library/limits/cli lanes;
* the R1 default-value spelling family from the validity lane (step B3).

Rows whose fixture cannot be compared with the P1/P2 oracle get
``kind: not-comparable`` and a reason.  They stay in the corpus so the 48-row
ledger is complete and so the summary can report what M1 did not cover.

Usage:
    python tests/codegen/harness/seed.py            # write the corpus
    python tests/codegen/harness/seed.py --check    # fail on drift
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys

import fds
import runner

HERE = pathlib.Path(__file__).resolve().parent
REPO = runner.repo_root(HERE)
ROWS_DIR = HERE / "corpus" / "rows"
REVIEW = HERE / "corpus" / "review"
TESTS = REPO / "tests/codegen"


# ---------------------------------------------------------------------------
# Review rows: one per CR, mapped to the fixture named in repro-index.md.
# ``root`` and ``cr`` come from build/verification/triage.csv.  ``p1_expect``
# records the verdict measured for the "before" fixture (oracle and product),
# and is documentation only: a row passes when the observed result is parity or
# when the divergence is explained by the row's root/exception.
# ---------------------------------------------------------------------------
REVIEW_ROWS = [
    # cr, kind, fixture, include_root, root, p1_expect, p2_expect, note
    ("CR-01", "source", "verify/float-octal.proto", "verify", "R1", "parity", "divergent",
     "float/double default spelled 010"),
    ("CR-02", "descriptor", "schema-descriptor/float_upper_inf.pbs", None, "R1", "parity", "n/a",
     "INF spelling reaches the header; P1 is parity, P3 catches the literal"),
    ("CR-03", "source", "emitter-names/f1.proto", "emitter-names", "R4", "parity", "parity",
     "enumerator vs presence-flag collision is a P3 defect"),
    ("CR-04", "source", "emitter-names/f4.proto", "emitter-names", "R4", "parity", "parity",
     "encode/decode missing from the file-scope check is a P3 defect"),
    ("CR-05", "descriptor", "schema-descriptor/uint64_max.pbs", None, "R1", "parity", "n/a",
     "extreme uint64 default is a P3 literal defect"),
    ("CR-06", "descriptor", "schema-descriptor/res_dup_name.pbs", None, None, "parity", "n/a",
     "reserved names/ranges are modelled and validated on both paths (B5)"),
    ("CR-07", "descriptor", "schema-descriptor/ext_high.pbs", None, None, "parity", "n/a",
     "the extension-range upper bound is enforced on both paths (B5)"),
    ("CR-08", "not-comparable", "emitter-names/thin.pbs", None, "R7", "n/a", "n/a",
     "thin/compatibility mode; the P1/P2 pilot invokes complete --target mode"),
    ("CR-09", "source", "emitter-names/f7.proto", "emitter-names", "R4", "parity", "parity",
     "top-level easypb/std collision is a P3 defect"),
    ("CR-10", "descriptor", "emitter-names/digit-field.pbs", None, "R4", "parity", "n/a",
     "non-identifier field name is accepted by protoc too; P3 catches the C++ name"),
    ("CR-11", "source", "parser-loader/oneof-default.proto", "parser-loader", "R5", "divergent", "n/a",
     "explicit default on a oneof field is refused by codegen, accepted by protoc"),
    ("CR-12", "source", "parser-loader/esc-octal.proto", "parser-loader", "R2", "divergent", "n/a",
     "octal escapes above 0xFF are refused"),
    ("CR-13", "source", "parser-loader/esc-surrogate.proto", "parser-loader", "R2", "divergent", "n/a",
     "UTF-16 surrogate escapes are refused"),
    ("CR-14", "not-comparable", None, None, "R8", "n/a", "n/a",
     "closed stdout pipe; CLI transcript property (P4/P5), no protoc analogue"),
    ("CR-15", "not-comparable", None, None, "R8", "n/a", "n/a",
     "options without input; CLI usage property (P4/P5)"),
    ("CR-16", "source", "verify/lroots/thing.proto", "verify/lroots", "R8", "parity", "parity",
     "the fixture itself resolves; the CR is about an input outside every root"),
    ("CR-17", "source", "verify/ispell.proto", "verify", "R1", "parity", "divergent",
     "hex/octal integer spellings differ between the two paths"),
    ("CR-18", "source", "verify/float-octal-08.proto", "verify", "R2", "divergent", "n/a",
     "invalid octal literal 08 is accepted"),
    ("CR-19", "source", "parser-loader/aggregate-nul.proto", "parser-loader", "R2", "divergent", "n/a",
     "NUL inside an aggregate option is swallowed"),
    ("CR-20", "source", "verify/leading-semicolon.proto", "verify", "R5", "divergent", "n/a",
     "leading empty statement is accepted"),
    ("CR-21", "descriptor", "schema-descriptor/map_entry_method.pbs", None, "R6", "divergent", "n/a",
     "method endpoint naming a synthetic map entry is refused; protoc accepts"),
    ("CR-22", "descriptor", "schema-descriptor/json_bracket.pbs", None, "R6", "divergent", "n/a",
     "custom json_name shaped like [...] is accepted; protoc refuses"),
    ("CR-23", "descriptor", "schema-descriptor/message_set.pbs", None, None, "parity", "n/a",
     "a MessageSet with fields is refused on the descriptor path (B5)"),
    ("CR-24", "descriptor", "schema-descriptor/merge_b_semantic.pbs", None, "R6", "parity", "n/a",
     "single-container invocation; the merge defect needs the CLI container pair"),
    ("CR-25", "source", "parser-loader/case-extend-unknown.proto", "parser-loader", "R3", "divergent", "n/a",
     "extension declarations are not modelled; accepted row, exception E6"),
    ("CR-26", "source", "verify/alias.proto", "verify", "R5", "divergent", "n/a",
     "allow_alias without shared numbers is accepted"),
    ("CR-27", "source", "verify/enumjson.proto", "verify", "R5", "divergent", "n/a",
     "json_name on an enum value is ignored"),
    ("CR-28", "source", "verify/option-plus.proto", "verify", "R5", "divergent", "n/a",
     "leading plus on an ordinary option value is accepted"),
    ("CR-29", "source", "verify/cr-string.proto", "verify", "R2", "divergent", "n/a",
     "raw CR inside a string literal is refused; protoc accepts"),
    ("CR-30", "source", "verify/nested-comment.proto", "verify", "R2", "divergent", "n/a",
     "nested block comments are accepted"),
    ("CR-31", "not-comparable", None, None, "R8", "n/a", "n/a",
     "Windows write failure; CLI/diagnostic property (P4/P5)"),
    ("CR-32", "not-comparable", None, None, "R8", "n/a", "n/a",
     "UNC --out-dir path; filesystem-safety property (P8)"),
    ("CR-33", "not-comparable", None, None, "R8", "n/a", "n/a",
     "empty -I value; CLI path rule (P1/P4), not a schema verdict"),
    ("CR-34", "not-comparable", None, None, "R8", "n/a", "n/a",
     "bare dash argument; CLI parsing property (P4/P5)"),
    ("CR-35", "descriptor", "emitter-names/chain12k.pbs", None, "R9", "parity", "n/a",
     "B11: the product accepts the 12 000-message chain; protoc 30.1 still "
     "crashes on it, so the one remaining crash is the oracle's. The no-crash "
     "property is executable in codegen.limits (dependency-chain)"),
    ("CR-36", "not-comparable", None, None, "R8", "n/a", "n/a",
     "110-component --out-dir; accepted P8 exception, not a schema verdict"),
    ("CR-37", "not-comparable", None, None, "R8", "n/a", "n/a",
     "--print-descriptor read failures; CLI diagnostic property (P4/P9)"),
    ("CR-38", "not-comparable", "cli-paths/UPPER.PBS", None, "R8", "n/a", "n/a",
     "UPPER.PBS is source text, not a descriptor set; the CR is mode inference (P4)"),
    ("CR-39", "not-comparable", None, None, "R8", "n/a", "n/a",
     "--print-descriptor refusal wording; CLI property (P9)"),
    ("CR-40", "not-comparable", None, None, "R8", "n/a", "n/a",
     "file in the middle of the output path; CLI property (P4/P9)"),
    ("CR-41", "not-comparable", None, None, "R8", "n/a", "n/a",
     "trailing separator on an input file; CLI property (P8)"),
    ("CR-42", "source", "verify/dupenum.proto", "verify", "R10", "parity", "n/a",
     "both tools refuse the duplicate enum number; the CR is diagnostic position (P9)"),
    ("CR-43", "not-comparable", "emitter-names/localname.cpp", None, "R4", "n/a", "n/a",
     "C++ unit fixture for cpp_local_name; needs a compile, not a schema verdict"),
    ("CR-44", "source", "emitter-names/f9.proto", "emitter-names", "R4", "parity", "parity",
     "accepted row; the insertion-macro stem collision is a P3 defect"),
    ("CR-45", "not-comparable", None, None, "R3", "n/a", "n/a",
     "crafted present-but-empty package; P2-only and no tracked fixture exists"),
    ("CR-46", "descriptor", "schema-descriptor/no_field_name.pbs", None, "R3", "parity", "n/a",
     "both tools refuse the missing field name; the CR is diagnostic wording (P9)"),
    ("CR-47", "not-comparable", None, None, "R8", "n/a", "n/a",
     "--benchmark-parser read failure; CLI exit-code property (P4/P5)"),
    ("CR-48", "not-comparable", None, None, "R10", "n/a", "n/a",
     "--print-descriptor services/methods; print-mode property (P9)"),
]

# Existing fixtures whose lane README declares them accepted by protoc and by
# the product.  The tuple is (lane, file stem, roots relative to the lane data
# dir); every entry is a `.proto` source.
LANE_CONTROLS = [
    ("enums", "enum_codegen", ["."]),
    ("enums", "open_enum", ["."]),
    ("maps", "enum_map", ["."]),
    ("maps", "message_map", ["."]),
    ("maps", "scalar_maps", ["."]),
    ("packages", "alpha", ["."]),
    ("packages", "beta", ["."]),
    ("packages", "external", ["."]),
    ("packages", "import-package", ["."]),
    ("packages", "module-package", ["."]),
    ("packages", "names", ["."]),
    ("packages", "other", ["."]),
    ("packages", "shadow", ["."]),
    ("library", "options-default", ["."]),
    ("library", "w47-external", ["."]),
    ("library", "w47-external-enum", ["."]),
    ("library", "w47-source-default", ["."]),
    ("library", "w48-wire", ["."]),
    ("custom_types", "custom_types", ["."]),
    ("custom_types", "template_cases", ["."]),
    ("names", "escape-field", ["."]),
    ("names", "escape-message", ["."]),
    ("names", "keyword-field", ["."]),
    ("names", "keyword-message", ["."]),
    ("names", "keyword-nested", ["."]),
    ("limits", "nesting-31", ["."]),
    ("cli", "defaults-empty", ["."]),
    ("parser", "integration-proto2", ["."]),
    ("parser", "integration-proto3", ["."]),
    ("parser", "nested-messages", ["."]),
    ("parser", "nested-forward", ["."]),
    ("parser", "nested-recursive", ["."]),
    ("parser", "service-rpc", ["."]),
]

DIFFERENTIAL_CORPUS = ["caffe.proto", "onnx.proto3", "mesos.proto", "PulsarApi.proto"]

# R1 / B3: the default-value spelling family. The source fixture and the
# protoc-written descriptor are one P1/P2 pair; the descriptor-only keyword
# fixture is a descriptor row because protoc's source grammar cannot spell
# `INF`. The fixtures live in the validity lane, next to the family test.
DEFAULT_FAMILY_ROWS = [
    ("defaults-spellings", "source"),
    ("defaults-spellings", "descriptor"),
    ("defaults-float-keywords", "descriptor"),
]

# R2 / B4: the lexer and literal family. Two paired accepted fixtures (the
# escape table and the lexeme table), one crafted descriptor set whose
# spellings only a descriptor can carry, and the source-only refusals.
# `protoc` refuses the last four at the same place the product must, and the
# comparison is verdict parity, not a header.
LITERAL_FAMILY_ROWS = [
    ("literals-escapes", "source", "parity",
     "escape table (hex, octal, \\u, \\U, surrogate, raw CR) on both kinds"),
    ("literals-escapes", "descriptor", "parity",
     "protoc's own descriptor for the escape table"),
    ("literals-lexemes", "source", "parity",
     "leading-zero lexeme table (0x10, 010, 077, 00, 0, 0.5, 0e5)"),
    ("literals-lexemes", "descriptor", "parity",
     "protoc's own descriptor for the lexeme table"),
    ("literals-descriptor-octal", "descriptor", "parity",
     "descriptor-carried 08/09/010.5: accepted by protoc's descriptor path"),
    ("literals-octal-08", "source", "refuse",
     "CR-18 float default 08"),
    ("literals-octal-09", "source", "refuse",
     "CR-18 ordinary option value 09"),
    ("literals-octal-enum", "source", "refuse",
     "CR-18 enum value 08"),
    ("literals-nested-comment", "source", "refuse",
     "CR-30 nested block comment"),
    ("literals-aggregate-nul", "source", "refuse",
     "CR-19 raw NUL in an aggregate option"),
]


def rel(path: pathlib.Path) -> str:
    return path.resolve().relative_to(REPO).as_posix()


def choose_root(input_path: pathlib.Path, roots: list[pathlib.Path]) -> pathlib.Path:
    for root in roots:
        try:
            input_path.resolve().relative_to(root.resolve())
            return root
        except ValueError:
            continue
    raise SystemExit("%s is not below any include root" % input_path)


def new_row(
    row_id: str,
    source: str,
    kind: str,
    *,
    property_: list[str],
    input_path: str | None = None,
    include_roots: list[str] | None = None,
    target: str | None = None,
    cr: str | None = None,
    root: str | None = None,
    exception: str | None = None,
    p1_expect: str = "n/a",
    p2_expect: str = "n/a",
    timeout_s: float = 10.0,
    mandatory: bool = False,
    reason: str | None = None,
    note: str | None = None,
    p3_applicable: bool | None = None,
    p3_note: str | None = None,
) -> dict:
    row = {
        "schema": 1,
        "id": row_id,
        "source": source,
        "kind": kind,
        "properties": property_,
        "input": input_path,
        "include_roots": include_roots or [],
        "target": target,
        "cr": cr,
        "root": root,
        "exception": exception,
        "p1_expect": p1_expect,
        "p2_expect": p2_expect,
        "timeout_s": timeout_s,
        "mandatory": mandatory,
        "reason": reason,
        "note": note,
    }
    if p3_applicable is not None:
        row["p3_applicable"] = p3_applicable
    if p3_note is not None:
        row["p3_note"] = p3_note
    return row


def review_rows() -> list[dict]:
    rows = []
    for (cr, kind, fixture, root_dir, root, p1, p2, note) in REVIEW_ROWS:
        row_id = cr
        if kind == "not-comparable":
            input_rel = rel(REVIEW / fixture) if fixture else None
            rows.append(new_row(
                row_id, "review", kind, property_=["P1", "P2"], cr=cr, root=root,
                p1_expect=p1, p2_expect=p2, mandatory=True,
                input_path=input_rel, reason=note, note=note,
            ))
            continue
        input_path = REVIEW / fixture
        if not input_path.is_file():
            raise SystemExit("%s: fixture missing: %s" % (row_id, input_path))
        include = [rel(REVIEW / root_dir)] if root_dir else []
        target = None
        properties = ["P1"]
        if kind == "source":
            properties = ["P1", "P2"]
            roots = [REVIEW / root_dir]
            logical = input_path.resolve().relative_to(
                choose_root(input_path, roots).resolve()
            ).as_posix()
        else:
            properties = ["P1"]
            files = fds.file_descriptors(input_path.read_bytes())
            target = fds.select_target(files)
            if not target:
                rows.append(new_row(
                    row_id, "review", "not-comparable", property_=["P1"], cr=cr,
                    root=root, mandatory=True,
                    reason="descriptor set has no file name; no target can be chosen",
                ))
                continue
            logical = target
        rows.append(new_row(
            row_id, "review", kind, property_=properties,
            input_path=rel(input_path), include_roots=include, target=target,
            cr=cr, root=root,
            exception="E6" if cr == "CR-25" else None,
            p1_expect=p1, p2_expect=p2,
            timeout_s=60.0 if cr == "CR-35" else 10.0,
            mandatory=True,
            note=note + " [logical name: %s]" % logical,
            p3_applicable=False if cr == "CR-35" else None,
            p3_note=(
                "generator stress fixture, not a compile fixture: 12 000 "
                "by-value messages produce a 9.5 MB / 384 009-line header; "
                "tests/codegen/limits/test_limits.cmake declares that header "
                "not compiled and P8 owns the chain walk. B15 measured the "
                "boundary: a 6 000-message chain compiles under g++ 15.2 in "
                "178 s, while 12 000 crashes cc1plus with STATUS_STACK_OVERFLOW "
                "(0xC00000FD) and does not finish under Clang 3.3."
            ) if cr == "CR-35" else None,
        ))
    return rows


# The ten ``inverse`` rows of parity-cases.tsv: protoc 30.1 accepts the shape,
# this project refuses it by an owner decision.  The P1 exception register
# carries the decision, so the rows are inside the parity baseline as
# ``exception``, not outside it as "not comparable".
INVERSE_EXCEPTIONS = {
    "s-unsupported-syntax-editions-inverse": "E8",
    "d-unsupported-syntax-editions-inverse": "E8",
    "d-unsupported-syntax-edition-2023-only-inverse": "E8",
    "d-unsupported-syntax-edition-2023-proto3-inverse": "E8",
    "d-invalid-import-public-index-duplicate-inverse": "E9",
    "d-invalid-import-public-and-weak-inverse": "E9",
    "d-malformed-map-entry-bytes-key-unreferenced-inverse": "E10",
    "d-malformed-map-entry-extension-range-unreferenced-inverse": "E10",
    "d-invalid-field-type-absent-reject": "E4",
    "d-invalid-field-type-group-reject": "E4",
}


def parity_tsv_rows() -> list[dict]:
    tsv = TESTS / "imports/data/parity/parity-cases.tsv"
    rows = []
    for line in tsv.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("case\t"):
            continue
        case, origin, protoc_verdict, expected, kind, note = line.split("\t")
        row_id = "t-parity-" + case
        if kind == "inverse":
            # Owner decisions (A18/A25/A26/W-10) deliberately refuse a shape
            # protoc accepts.  Each row carries the exception ID that records
            # the decision (property-catalogue.md P1 exception list).
            exception = INVERSE_EXCEPTIONS.get(case)
            if exception is None:
                raise SystemExit(
                    "%s: inverse row has no exception ID" % case)
            if origin == "source":
                input_path = (TESTS / "imports/data/parity/source"
                              / (case + ".proto"))
                rows.append(new_row(
                    row_id, "test", "source", property_=["P1"],
                    input_path=rel(input_path),
                    include_roots=[rel(TESTS / "imports/data/parity/source")],
                    exception=exception, p1_expect="stricter",
                    note="inverse row (declared protoc verdict: %s)" % protoc_verdict,
                ))
            else:
                input_path = (TESTS / "imports/data/parity/set"
                              / (case + ".pbs"))
                files = fds.file_descriptors(input_path.read_bytes())
                target = fds.select_target(files, preferred="root.proto")
                if not target:
                    raise SystemExit(
                        "%s: inverse set has no file name" % case)
                rows.append(new_row(
                    row_id, "test", "descriptor", property_=["P1"],
                    input_path=rel(input_path), target=target,
                    exception=exception, p1_expect="stricter",
                    note="inverse row (declared protoc verdict: %s)" % protoc_verdict,
                ))
            continue
        if origin == "source":
            input_path = TESTS / "imports/data/parity/source" / (case + ".proto")
            rows.append(new_row(
                row_id, "test", "source", property_=["P1", "P2"],
                input_path=rel(input_path),
                include_roots=[rel(TESTS / "imports/data/parity/source")],
                p1_expect="parity", p2_expect="parity",
                note="declared protoc verdict: %s" % protoc_verdict,
            ))
        else:
            input_path = TESTS / "imports/data/parity/set" / (case + ".pbs")
            files = fds.file_descriptors(input_path.read_bytes())
            target = fds.select_target(files, preferred="root.proto")
            if not target:
                rows.append(new_row(
                    row_id, "test", "not-comparable", property_=["P1"],
                    reason="descriptor set has no file name",
                    note="declared protoc verdict: %s" % protoc_verdict,
                ))
                continue
            rows.append(new_row(
                row_id, "test", "descriptor", property_=["P1"],
                input_path=rel(input_path), target=target,
                p1_expect="parity",
                note="declared protoc verdict: %s" % protoc_verdict,
            ))
    return rows


# CR-25 is accepted (E6): extension declarations are neither validated nor
# decoded. The three fixtures that pin that direction live in the parity lane
# and are carried in the corpus with the exception ID, so the harness records
# the accepted divergence instead of a new root. `codegen.parity.
# extensions_not_validated` is the executable xfail; these rows are its
# differential measurement.
EXTENSION_XFAIL_STEMS = frozenset(
    {"ext-unvalidated", "ext-member-no-extendee", "ext-member-out-of-range"})


def parity_lane_rows() -> list[dict]:
    data = TESTS / "parity/data"
    rows = []
    for path in sorted(data.glob("*.proto")):
        if path.name.startswith("w36-"):
            continue
        if path.stem in EXTENSION_XFAIL_STEMS:
            rows.append(new_row(
                "t-lane-parity-source-" + path.stem, "test", "source",
                property_=["P1"], input_path=rel(path),
                include_roots=[rel(data)], cr="CR-25", exception="E6",
                p1_expect="divergent", p2_expect="n/a",
                note="CR-25 accepted (E6): extension declarations are not "
                     "validated",
            ))
            continue
        rows.append(new_row(
            "t-lane-parity-source-" + path.stem, "test", "source",
            property_=["P1", "P2"], input_path=rel(path),
            include_roots=[rel(data)], p1_expect="parity", p2_expect="parity",
            note="cross-path parity lane fixture",
        ))
    for path in sorted(data.glob("*.pbs")):
        files = fds.file_descriptors(path.read_bytes())
        target = fds.select_target(files)
        if not target:
            rows.append(new_row(
                "t-lane-parity-descriptor-" + path.stem, "test", "not-comparable",
                property_=["P1"], reason="descriptor set has no file name",
            ))
            continue
        if path.stem in EXTENSION_XFAIL_STEMS:
            rows.append(new_row(
                "t-lane-parity-descriptor-" + path.stem, "test", "descriptor",
                property_=["P1"], input_path=rel(path), target=target,
                cr="CR-25", exception="E6", p1_expect="divergent",
                note="CR-25 accepted (E6): extension declarations are not "
                     "validated",
            ))
            continue
        rows.append(new_row(
            "t-lane-parity-descriptor-" + path.stem, "test", "descriptor", property_=["P1"],
            input_path=rel(path), target=target, p1_expect="parity",
            note="cross-path parity lane fixture",
        ))
    return rows


def differential_rows() -> list[dict]:
    corpus = TESTS / "parser/differential/corpus"
    rows = []
    for name in DIFFERENTIAL_CORPUS:
        path = corpus / name
        rows.append(new_row(
            "t-diff-" + path.stem, "test", "source", property_=["P1", "P2"],
            input_path=rel(path), include_roots=[rel(corpus)],
            p1_expect="parity", p2_expect="parity", timeout_s=60.0,
            note="existing descriptor-level differential corpus",
        ))
    return rows


def lane_control_rows() -> list[dict]:
    rows = []
    for lane, stem, roots in LANE_CONTROLS:
        data = TESTS / lane / "data"
        path = data / (stem + ".proto")
        if not path.is_file():
            raise SystemExit("lane control missing: %s" % path)
        include = [rel(data / part) for part in roots]
        rows.append(new_row(
            "t-lane-%s-%s" % (lane, stem), "test", "source",
            property_=["P1", "P2"], input_path=rel(path), include_roots=include,
            p1_expect="parity", p2_expect="parity",
            note="accepted control of the %s lane" % lane,
        ))
    project = TESTS / "imports/data/project"
    rows.append(new_row(
        "t-lane-imports-project-report", "test", "source",
        property_=["P1", "P2"],
        input_path=rel(project / "api/report.proto"),
        include_roots=[rel(project)], p1_expect="parity", p2_expect="parity",
        note="multi-root import project",
    ))
    return rows


def default_family_rows() -> list[dict]:
    """The R1 default-value spelling family added by step B3."""
    data = TESTS / "validity/data"
    rows = []
    for stem, kind in DEFAULT_FAMILY_ROWS:
        if kind == "source":
            rows.append(new_row(
                "t-default-%s-source" % stem, "test", "source",
                property_=["P1", "P2"],
                input_path=rel(data / (stem + ".proto")),
                include_roots=[rel(data)],
                p1_expect="parity", p2_expect="parity",
                note="R1 default-value spelling family: source path (P2 owns "
                     "the cross-path verdict; compile.py owns P3)",
            ))
            continue
        path = data / (stem + ".pbs")
        target = fds.select_target(fds.file_descriptors(path.read_bytes()))
        if not target:
            raise SystemExit("no logical target in %s" % path)
        rows.append(new_row(
            "t-default-%s-descriptor" % stem, "test", "descriptor",
            property_=["P1"], input_path=rel(path), target=target,
            p1_expect="parity",
            note="R1 default-value spelling family: descriptor path "
                 "(compile.py owns the P3 verdict)",
        ))
    return rows


def literal_family_rows() -> list[dict]:
    """The R2 lexer and literal family added by step B4."""
    data = TESTS / "validity/data"
    rows = []
    for stem, kind, verdict, note in LITERAL_FAMILY_ROWS:
        if kind == "source":
            rows.append(new_row(
                "t-literal-%s-source" % stem, "test", "source",
                property_=["P1", "P2"],
                input_path=rel(data / (stem + ".proto")),
                include_roots=[rel(data)],
                p1_expect=verdict,
                p2_expect="parity" if verdict == "parity" else "n/a",
                note="R2 lexer/literal family: source path - " + note,
            ))
            continue
        path = data / (stem + ".pbs")
        target = fds.select_target(fds.file_descriptors(path.read_bytes()))
        if not target:
            raise SystemExit("no logical target in %s" % path)
        rows.append(new_row(
            "t-literal-%s-descriptor" % stem, "test", "descriptor",
            property_=["P1"], input_path=rel(path), target=target,
            p1_expect=verdict,
            note="R2 lexer/literal family: descriptor path - " + note,
        ))
    return rows


def discovered_rows() -> list[dict]:
    """Rows written by the M2 discovery driver and the B2b hand-built plan.

    The discovery driver owns the ``source: generated`` sidecars and their
    minimized inputs under ``corpus/generated/<id>/``; the hand-built plan owns
    the ``source: handbuilt`` ones.  This function only folds them into the
    corpus index so ``--check`` and a full run cover the regression set.
    """
    rows: list[dict] = []
    for path in sorted(ROWS_DIR.glob("*.expect.json")):
        with open(path, encoding="utf-8") as handle:
            row = json.load(handle)
        if row.get("source") in ("generated", "handbuilt"):
            rows.append(row)
    return rows


def build_rows() -> list[dict]:
    rows = review_rows() + parity_tsv_rows() + parity_lane_rows()
    rows += differential_rows() + lane_control_rows() + discovered_rows()
    rows += default_family_rows() + literal_family_rows()
    seen: set[str] = set()
    for row in rows:
        if row["id"] in seen:
            raise SystemExit("duplicate row id: %s" % row["id"])
        seen.add(row["id"])
    rows.sort(key=lambda row: row["id"])
    return rows


def serialise(row: dict) -> str:
    return json.dumps(row, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="compare the tree with the generated rows")
    args = parser.parse_args()

    rows = build_rows()
    if args.check:
        expected = {row["id"] + ".expect.json": serialise(row) for row in rows}
        actual = {path.name: path.read_text(encoding="utf-8")
                  for path in ROWS_DIR.glob("*.expect.json")}
        missing = sorted(set(expected) - set(actual))
        extra = sorted(set(actual) - set(expected))
        changed = sorted(name for name in set(expected) & set(actual)
                         if expected[name] != actual[name])
        if missing or extra or changed:
            for name in missing:
                print("missing: %s" % name)
            for name in extra:
                print("extra: %s" % name)
            for name in changed:
                print("changed: %s" % name)
            return 1
        print("corpus rows match: %d" % len(rows))
        return 0

    ROWS_DIR.mkdir(parents=True, exist_ok=True)
    for stale in ROWS_DIR.glob("*.expect.json"):
        stale.unlink()
    for row in rows:
        path = ROWS_DIR / (row["id"] + ".expect.json")
        path.write_text(serialise(row), encoding="utf-8", newline="\n")

    comparable = [row for row in rows if row["kind"] != "not-comparable"]
    kinds: dict[str, int] = {}
    sources: dict[str, int] = {}
    for row in rows:
        kinds[row["kind"]] = kinds.get(row["kind"], 0) + 1
        sources[row["source"]] = sources.get(row["source"], 0) + 1
    print("wrote %d rows to %s" % (len(rows), rel(ROWS_DIR)))
    print("  comparable: %d" % len(comparable))
    print("  kinds: %s" % kinds)
    print("  sources: %s" % sources)
    return 0


if __name__ == "__main__":
    sys.exit(main())
