"""B2/M2 discovery slice: generate cases, run them, classify the noise.

This is deliberately not the full M2.  It adds the two input producers
(``generate.py`` and ``mutate.py``), runs their 200 cases through the existing
runner and P1/P2 comparator, and classifies every case as one of:

* ``pass`` — codegen and protoc agree (and the two paths agree where P2 applies);
* ``known root`` — the divergence is a new instance of a triage root R1–R10;
* ``allowed-stricter`` — a documented P1 exception or policy;
* ``new root`` — no explanation; recorded in ``pilot-findings.md`` and the run
  stops before any fix;
* ``false positive`` — the comparator's own invariants are violated.

New instances of known roots are copied into ``corpus/generated/<id>/`` with a
``corpus/rows/<id>.expect.json`` sidecar.  New roots are never added to the
corpus: the owner decides whether the triage grows.

This driver deliberately does not shrink, compile or check P7: those are the
separate B2b commands ``shrink.py``, ``compile.py`` and ``thin.py``, and no
change to the product or to ``tests/`` is made here.

Usage:
    python tests/codegen/harness/discover.py --cases 200 --seed 1
    python tests/codegen/harness/discover.py --cases 200 --seed 1 --quiet
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import re
import shutil
import sys
import time

import generate
import run
import runner
import seed

HERE = pathlib.Path(__file__).resolve().parent
REPO = runner.repo_root(HERE)
GENERATED_ROOT = HERE / "generated"
CORPUS_GENERATED = HERE / "corpus" / "generated"
CORPUS_ROWS = HERE / "corpus" / "rows"
PILOT_FINDINGS = HERE / "pilot-findings.md"
SUMMARY = HERE / "discovery-summary.json"
ROOTS = {"R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "R10"}


# ---------------------------------------------------------------------------
# Case -> runner row
# ---------------------------------------------------------------------------


def relative(path: pathlib.Path) -> str:
    return path.resolve().relative_to(REPO).as_posix()


def case_row(case: generate.Case) -> dict:
    kind = "descriptor" if case.mode == "descriptor-mutation" else "source"
    return {
        "schema": 1,
        "id": case.id,
        "source": "generated",
        "kind": kind,
        "properties": ["P1"] if kind == "descriptor" else ["P1", "P2"],
        "input": None,
        "include_roots": [],
        "target": case.target if kind == "descriptor" else None,
        "cr": None,
        "root": None,
        "exception": case.exception,
        "p1_expect": "n/a",
        "p2_expect": "n/a",
        "timeout_s": 10.0,
        "mandatory": False,
        "reason": None,
        "note": "%s: %s" % (case.mechanism, case.detail),
        "generated": {
            "seed": None,
            "mode": case.mode,
            "family": case.family,
            "mechanism": case.mechanism,
            "root_hint": case.root,
            "oracle_expect": case.oracle_expect,
        },
    }


def build_row(case: generate.Case, seed_value: int) -> dict:
    directory = GENERATED_ROOT / ("s%d" % seed_value) / "cases" / case.id
    row = case_row(case)
    input_name = case.target if row["kind"] == "source" else next(iter(case.files))
    row["input"] = relative(directory / input_name)
    if row["kind"] == "source":
        row["include_roots"] = [relative(directory)]
    row["generated"]["seed"] = seed_value
    return row


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------


def _event(result: dict) -> dict:
    p1 = result.get("p1") or {}
    p2 = result.get("p2") or {}
    return {
        "p1": p1,
        "p2": p2,
        "p1_match": p1.get("match"),
        "p2_match": p2.get("match"),
        "p2_applicable": bool(p2.get("applicable")),
        "codegen_accept": p1.get("codegen_accept"),
        "protoc_accept": p1.get("protoc_accept"),
    }


def _mechanism_root(case: generate.Case, event: dict, result: dict) -> str | None:
    """Return the triage root that owns the observed divergence, if any."""
    for tool in ("codegen", "protoc"):
        capture = result.get(tool) or {}
        if capture.get("crashed") or capture.get("timed_out"):
            # A one-sided crash or timeout is a resource-bound event (R9), not
            # the family mechanism the case was built to probe.
            return None
    mechanism = case.mechanism
    p1_diverged = event["p1_match"] is False
    p2_diverged = event["p2_applicable"] and event["p2_match"] is False
    if not (p1_diverged or p2_diverged):
        return None
    if mechanism.startswith("r2-"):
        return "R2"
    if mechanism.startswith("r5-"):
        return "R5"
    if mechanism.startswith("d-"):
        root = case.root
        return root if root in ROOTS else None
    if p2_diverged and not p1_diverged:
        # CR-01/CR-17: both paths accept, the header differs, and the case
        # carries a default-value literal.  The first differing line is kept as
        # evidence by the caller.
        if any(tag.startswith("default.") for tag in case.coverage):
            return "R1"
        return None
    return None


def _allowed_stricter(case: generate.Case, result: dict) -> dict | None:
    p1 = result.get("p1") or {}
    if p1.get("match") is not False:
        return None
    codegen_accept = p1.get("codegen_accept")
    protoc_accept = p1.get("protoc_accept")
    if codegen_accept is not False or protoc_accept is not True:
        return None
    classification = p1.get("classification")
    if classification == "allowed-stricter":
        return {
            "policy": p1.get("policy"),
            "authority": p1.get("authority"),
        }
    if classification == "exception" and case.exception:
        return {
            "policy": "exception/%s" % case.exception,
            "authority": "property-catalogue.md P1 exception list",
        }
    if case.mechanism == "d-clear-field-type":
        return {
            "policy": "exception/E4",
            "authority": "property-catalogue.md E4: absent field kinds are "
                         "refused on the descriptor path",
        }
    return None


def _invariant(case: generate.Case, result: dict) -> str | None:
    """A comparator invariant that must hold; a violation is a false positive."""
    p1 = result.get("p1") or {}
    p2 = result.get("p2") or {}
    for tool in ("codegen", "protoc"):
        stderr = (result.get(tool) or {}).get("stderr") or ""
        if "cannot read file" in stderr or "No such file" in stderr:
            return "%s could not read the case input: %s" % (
                tool, stderr.strip().splitlines()[0][:200])
    if p1.get("match") is True and p1.get("classification") != "pass":
        return "P1 match=True but classification=%r" % p1.get("classification")
    if p1.get("match") is False and p1.get("classification") == "pass":
        return "P1 match=False but classification=pass"
    if p2.get("applicable") and p2.get("match") is True:
        if p2.get("source_sha256") != p2.get("descriptor_sha256"):
            return "P2 match=True but the header hashes differ"
    if (case.mode != "descriptor-mutation"
            and p1.get("codegen_accept") is True
            and p1.get("protoc_accept") is True
            and p2.get("applicable") is not True):
        return "P2 not applicable although both source paths accepted"
    if p1.get("codegen_accept") == p1.get("protoc_accept") and p1.get("match") is False:
        return "P1 match=False although the verdicts agree"
    return None


def classify_case(case: generate.Case, result: dict) -> dict:
    classification: dict = {
        "id": case.id,
        "mode": case.mode,
        "family": case.family,
        "mechanism": case.mechanism,
        "root_hint": case.root,
        "detail": case.detail,
        "descriptor_path": case.descriptor_path,
        "input": result.get("input"),
        "target": result.get("logical_name"),
    }
    classification["crashes"] = [
        tool for tool in ("codegen", "protoc", "codegen_from_pbs")
        if (result.get(tool) or {}).get("crashed")
    ]
    classification["timeouts"] = [
        tool for tool in ("codegen", "protoc", "codegen_from_pbs")
        if (result.get(tool) or {}).get("timed_out")
    ]
    if result.get("runner_error"):
        classification.update({
            "class": "runner-error",
            "reason": result["runner_error"],
        })
        return classification
    if case.mode == "valid" and case.oracle_expect == "accept":
        protoc_capture = result.get("protoc") or {}
        if (result.get("p1", {}).get("protoc_accept") is not True
                and not protoc_capture.get("crashed")):
            classification.update({
                "class": "generator-error",
                "reason": "valid-by-construction case rejected by protoc: %s"
                          % _first_line(result, "protoc"),
            })
            return classification
    if case.exception == "E5" and result.get("p1", {}).get("protoc_accept") is not True:
        classification.update({
            "class": "generator-error",
            "reason": "E5 case is not accepted by protoc: %s"
                      % _first_line(result, "protoc"),
        })
        return classification
    invariant = _invariant(case, result)
    if invariant:
        classification.update({
            "class": "false-positive",
            "reason": invariant,
        })
        return classification
    event = _event(result)
    if event["p1_match"] is True and (
            not event["p2_applicable"] or event["p2_match"] is True):
        classification.update({
            "class": "pass",
            "p1": result.get("p1", {}).get("classification"),
            "p2": result.get("p2", {}).get("classification"),
        })
        return classification
    allowed = _allowed_stricter(case, result)
    if allowed:
        classification.update({
            "class": "allowed-stricter",
            "policy": allowed["policy"],
            "authority": allowed["authority"],
            "p1": result.get("p1", {}).get("classification"),
            "p2": result.get("p2", {}).get("classification"),
        })
        return classification
    root = _mechanism_root(case, event, result)
    if root:
        classification.update({
            "class": "known-root",
            "root": root,
            "p1": result.get("p1", {}).get("classification"),
            "p2": result.get("p2", {}).get("classification"),
            "codegen_accept": event["codegen_accept"],
            "protoc_accept": event["protoc_accept"],
            "p2_applicable": event["p2_applicable"],
            "p2_first_difference": (result.get("p2") or {}).get("first_difference"),
            "codegen_stderr": _first_line(result, "codegen"),
            "protoc_stderr": _first_line(result, "protoc"),
        })
        return classification
    classification.update({
        "class": "new-root",
        "p1": result.get("p1", {}).get("classification"),
        "p2": result.get("p2", {}).get("classification"),
        "codegen_accept": event["codegen_accept"],
        "protoc_accept": event["protoc_accept"],
        "p2_applicable": event["p2_applicable"],
        "p2_first_difference": (result.get("p2") or {}).get("first_difference"),
        "codegen_stderr": _first_line(result, "codegen"),
        "protoc_stderr": _first_line(result, "protoc"),
        "signature": _signature(case, result),
    })
    return classification


def _first_line(result: dict, tool: str) -> str | None:
    capture = result.get(tool) or {}
    stderr = (capture.get("stderr") or "").strip()
    if not stderr:
        return None
    return stderr.splitlines()[0][:400]


def _signature(case: generate.Case, result: dict) -> str:
    p1 = result.get("p1") or {}
    p2 = result.get("p2") or {}
    first = _first_line(result, "codegen") or _first_line(result, "protoc") or ""
    normalized = re.sub(r"[0-9]+", "N", first)
    return "%s|%s|P1=%s/%s|P2=%s|%s" % (
        case.family,
        case.mechanism,
        p1.get("codegen_accept"),
        p1.get("protoc_accept"),
        p2.get("match") if p2.get("applicable") else "n/a",
        normalized,
    )


# ---------------------------------------------------------------------------
# Corpus rows
# ---------------------------------------------------------------------------


def save_known_instance(case: generate.Case, seed_value: int,
                        classification: dict) -> str:
    directory = GENERATED_ROOT / ("s%d" % seed_value) / "cases" / case.id
    destination = CORPUS_GENERATED / case.id
    sidecar = CORPUS_ROWS / (case.id + ".expect.json")
    if sidecar.is_file():
        existing = json.loads(sidecar.read_text(encoding="utf-8"))
        if (existing.get("minimized") or {}).get("verified"):
            # B2b's shrinker owns this row now: never replace a minimized input
            # with the raw generator output on a later discovery run.
            return relative(sidecar)
    if destination.is_dir():
        shutil.rmtree(destination)
    destination.mkdir(parents=True, exist_ok=True)
    for name, payload in case.files.items():
        (destination / name).write_bytes(payload)
    kind = "descriptor" if case.mode == "descriptor-mutation" else "source"
    input_name = case.target if kind == "source" else next(iter(case.files))
    properties = ["P1"] if (kind == "descriptor"
                             or classification.get("p1") != "pass") else ["P2"]
    row = {
        "schema": 1,
        "id": case.id,
        "source": "generated",
        "kind": kind,
        "properties": properties,
        "input": relative(destination / input_name),
        "include_roots": [relative(destination)] if kind == "source" else [],
        "target": case.target if kind == "descriptor" else None,
        "cr": None,
        "root": classification["root"],
        "exception": None,
        "p1_expect": "divergent" if classification.get("p1") != "pass" else "parity",
        "p2_expect": ("n/a" if classification.get("p2") in (None, "n/a")
                      else "parity" if classification.get("p2") == "pass"
                      else "divergent"),
        "timeout_s": 10.0,
        "mandatory": True,
        "reason": None,
        "note": "M2 discovery seed %d, %s: %s" % (
            seed_value, case.mechanism, case.detail),
        "generated": {
            "seed": seed_value,
            "case": case.id,
            "mechanism": case.mechanism,
            "root_hint": case.root,
            "classification": classification.get("class"),
        },
    }
    CORPUS_ROWS.mkdir(parents=True, exist_ok=True)
    sidecar.write_text(seed.serialise(row), encoding="utf-8", newline="\n")
    return relative(sidecar)


# ---------------------------------------------------------------------------
# Summary and findings
# ---------------------------------------------------------------------------


def _coverage(cases: list[generate.Case], classifications: list[dict]) -> dict:
    productions: set[str] = set()
    options: set[str] = set()
    descriptor_fields: set[str] = set()
    families: dict[str, int] = {}
    for case in cases:
        for tag in case.coverage:
            if tag.startswith("option."):
                options.add(tag[len("option."):])
            else:
                productions.add(tag)
        if case.descriptor_path:
            descriptor_fields.add(case.descriptor_path)
        families[case.family] = families.get(case.family, 0) + 1
    return {
        "generator_tags": len(productions),
        "generator_tag_list": sorted(productions),
        "descriptor_paths": len(descriptor_fields),
        "descriptor_path_list": sorted(descriptor_fields),
        "options": len(options),
        "option_names": sorted(options),
        "families": families,
        "note": "generator tags and descriptor fields touched; the B1 register "
                "denominators are reported under inventory",
        "b1_denominators": {"grammar": 90, "descriptor_fields": 171, "cli": 61},
    }


def build_summary(cases: list[generate.Case], classifications: list[dict],
                  duration_s: float, seed_value: int, run_dir: pathlib.Path,
                  args, selection: str, corpus_rows: list[str],
                  inventory: dict | None) -> dict:
    def of(class_name: str) -> list[dict]:
        return [item for item in classifications if item["class"] == class_name]

    known = of("known-root")
    new = of("new-root")
    false_positives = of("false-positive")
    allowed = of("allowed-stricter")
    passes = of("pass")
    generator_errors = of("generator-error")
    runner_errors = of("runner-error")
    crashes = [
        {"id": item["id"], "tools": item["crashes"]}
        for item in classifications if item["crashes"]
    ]
    timeouts = [
        {"id": item["id"], "tools": item["timeouts"]}
        for item in classifications if item["timeouts"]
    ]
    by_root: dict[str, list[str]] = {}
    for item in known:
        by_root.setdefault(item["root"], []).append(item["id"])
    new_groups: dict[str, list[dict]] = {}
    for item in new:
        new_groups.setdefault(item["signature"], []).append(item)
    p1_mismatch = sum(
        1 for item in classifications
        if item.get("p1") not in (None, "pass"))
    p2_mismatch = sum(
        1 for item in classifications
        if item.get("p2") not in (None, "pass", "n/a"))
    mismatch_cases = sum(
        1 for item in classifications
        if item.get("p1") not in (None, "pass")
        or item.get("p2") not in (None, "pass", "n/a"))
    summary = {
        "schema": 1,
        "step": "B2",
        "milestone": "M2",
        "slice": "discovery",
        "run_id": run_dir.name,
        "started_at": args._started_at,
        "finished_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "duration_s": round(duration_s, 2),
        "command": args._command,
        "seed": seed_value,
        "cases": len(cases),
        "cases_requested": args.cases,
        "selection": selection,
        "counts": {
            "pass": len(passes),
            "known-root": len(known),
            "allowed-stricter": len(allowed),
            "new-root": len(new),
            "false-positive": len(false_positives),
            "generator-error": len(generator_errors),
            "runner-error": len(runner_errors),
        },
        "verdict_mismatches": mismatch_cases,
        "p1_mismatches": p1_mismatch,
        "p2_mismatches": p2_mismatch,
        "compile_failures": None,
        "compile_failures_note": "P3 is outside the M2 discovery slice",
        "new_roots": len(new_groups),
        "known_instances": len(known),
        "known_by_root": {root: sorted(ids) for root, ids in sorted(by_root.items())},
        "known_instances_detail": known,
        "allowed_stricter": len(allowed),
        "allowed_stricter_detail": allowed,
        "false_positives": len(false_positives),
        "false_positives_detail": false_positives,
        "generator_errors": len(generator_errors),
        "generator_errors_detail": generator_errors,
        "runner_errors": len(runner_errors),
        "runner_errors_detail": runner_errors,
        "new_roots_detail": [
            {
                "signature": signature,
                "cases": sorted(item["id"] for item in items),
                "mechanism": items[0]["mechanism"],
                "family": items[0]["family"],
                "proposed_mechanism": items[0]["detail"],
                "evidence": items,
            }
            for signature, items in sorted(new_groups.items())
        ],
        "corpus_rows": sorted(corpus_rows),
        "crashes": crashes,
        "timeouts": timeouts,
        "coverage": _coverage(cases, classifications),
        "inventory": inventory,
        "tools": run.tool_version(pathlib.Path(args.protoc).resolve(),
                                  pathlib.Path(args.codegen).resolve()),
        "results_file": relative(run_dir / "results.json"),
        "case_results": [
            {
                "id": item["id"],
                "family": item["family"],
                "mechanism": item["mechanism"],
                "class": item["class"],
                "root": item.get("root"),
                "policy": item.get("policy"),
                "p1": item.get("p1"),
                "p2": item.get("p2"),
            }
            for item in classifications
        ],
    }
    return summary


def write_json(path: pathlib.Path, payload: dict) -> None:
    path.write_text(
        json.dumps(payload, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8", newline="\n",
    )


def write_findings(summary: dict, run_dir: pathlib.Path,
                   codegen: pathlib.Path, protoc: pathlib.Path) -> None:
    new_roots = summary.get("new_roots_detail") or []
    if not new_roots:
        return
    stamp = dt.datetime.now().astimezone().strftime("%Y-%m-%d %H:%M")
    lines: list[str] = []
    if not PILOT_FINDINGS.is_file():
        lines += [
            "# Pilot findings — B2/M2 discovery",
            "",
            "Each entry is a divergence that the P1/P2 comparator could not",
            "explain by R1–R10, an E1–E6 exception or an allowed-stricter",
            "policy.  No fix is attempted: the owner decides whether the",
            "triage grows.",
            "",
        ]
    lines.append("## %s — %d new root candidate(s), run `%s`" % (
        stamp, len(new_roots), summary["run_id"]))
    lines.append("")
    for number, root in enumerate(new_roots, start=1):
        lines.append("### N%d — %s" % (number, root["mechanism"]))
        lines.append("")
        lines.append("- family: `%s`; proposed mechanism: %s" % (
            root["family"], root["proposed_mechanism"]))
        lines.append("- cases: %s" % ", ".join("`%s`" % item
                                              for item in root["cases"]))
        first = root["evidence"][0]
        lines.append("- observed: P1 codegen=%s protoc=%s; P2 applicable=%s match=%s" % (
            first.get("codegen_accept"), first.get("protoc_accept"),
            first.get("p2_applicable"), first.get("p2_match", "n/a")))
        if first.get("codegen_stderr"):
            lines.append("- codegen: `%s`" % first["codegen_stderr"])
        if first.get("protoc_stderr"):
            lines.append("- protoc: `%s`" % first["protoc_stderr"])
        if first.get("p2_first_difference"):
            lines.append("- first differing line: `%s`" % json.dumps(
                first["p2_first_difference"], ensure_ascii=True))
        case_dir = GENERATED_ROOT / ("s%d" % summary["seed"]) / "cases" / root["cases"][0]
        input_path = REPO / first["input"] if first.get("input") else case_dir
        lines.append("- input: `%s`" % (relative(case_dir) if first.get("input") else relative(input_path)))
        lines.append("- reproduction:")
        lines.append("")
        lines.append("  ```sh")
        if first.get("mechanism", "").startswith("d-"):
            lines.append("  %s --descriptor-set --target %s --out-dir <out> %s" % (
                codegen, first.get("target"), input_path))
            lines.append("  %s --descriptor_set_in=%s --cpp_out=<out> %s" % (
                protoc, input_path, first.get("target")))
        else:
            lines.append("  %s -I %s --out-dir <out> %s" % (
                codegen, case_dir, input_path))
            lines.append("  %s -I %s --cpp_out=<out> "
                         "--descriptor_set_out=<out>/out.pbs %s" % (
                             protoc, case_dir, input_path))
        lines.append("  ```")
        lines.append("")
    lines.append("- run summary: `%s`" % relative(run_dir / "summary.json"))
    lines.append("")
    PILOT_FINDINGS.write_text("\n".join(lines) + "\n", encoding="utf-8",
                              newline="\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cases", type=int, default=200)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--protoc", type=pathlib.Path, default=run.DEFAULT_PROTOC)
    parser.add_argument("--codegen", type=pathlib.Path, default=run.DEFAULT_CODEGEN)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--out", type=pathlib.Path, default=None)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()
    args.protoc = runner.resolve_tool(args.protoc)
    args.codegen = runner.resolve_tool(args.codegen)
    started = time.monotonic()
    args._started_at = dt.datetime.now().astimezone().isoformat(timespec="seconds")
    args._command = [sys.executable] + sys.argv
    if not args.protoc.is_file():
        print("protoc not found: %s" % args.protoc, file=sys.stderr)
        return 2
    if not args.codegen.is_file():
        print("codegen not found: %s" % args.codegen, file=sys.stderr)
        return 2

    generated = GENERATED_ROOT / ("s%d" % args.seed)
    cases = generate.generate_cases(args.seed, args.cases, args.protoc,
                                    generated / "_base")
    manifest = generate.write_cases(cases, generated)
    print("generated: %d cases under %s (digest %s)" % (
        len(cases), relative(generated), generate.manifest_digest(manifest)))

    run_id = dt.datetime.now().strftime("m2-%Y%m%d-%H%M%S")
    run_dir = (args.out or (HERE / "runs" / run_id)).resolve()
    run_dir.mkdir(parents=True, exist_ok=True)
    print("run: %s" % run_dir)
    print("tools: protoc=%s codegen=%s" % (args.protoc, args.codegen))

    rows = [build_row(case, args.seed) for case in cases]
    results: dict[str, dict] = {}
    for index, (case, row) in enumerate(zip(cases, rows), start=1):
        input_path = REPO / row["input"]
        if not input_path.is_file():
            raise SystemExit("generated input missing: %s" % input_path)
        item = run.run_case(row, run_dir, args.codegen.resolve(),
                            args.protoc.resolve(), args.timeout)
        item["case"] = {
            "mode": case.mode,
            "family": case.family,
            "mechanism": case.mechanism,
            "detail": case.detail,
            "oracle_expect": case.oracle_expect,
            "root_hint": case.root,
            "exception": case.exception,
            "descriptor_path": case.descriptor_path,
        }
        results[case.id] = item
        case_file = run_dir / case.id / "result.json"
        run.write_json(case_file, item)
        if not args.quiet:
            p1 = item.get("p1", {}).get("classification", "-")
            p2 = item.get("p2", {}).get("classification", "-")
            print("[%3d/%3d] %-28s %-14s P1=%-14s P2=%-10s" % (
                index, len(cases), case.id, case.mode, p1, p2))

    classifications = [classify_case(case, results[case.id]) for case in cases]
    corpus_rows: list[str] = []
    for case, classification in zip(cases, classifications):
        if classification["class"] == "known-root":
            corpus_rows.append(save_known_instance(case, args.seed, classification))
    duration = time.monotonic() - started

    selection = "fixed plan: 120 valid, 62 source mutations, 18 descriptor mutations"
    write_json(run_dir / "results.json", {
        "results": [results[case.id] for case in cases],
        "classifications": classifications,
    })
    inventory = run.inventory_counters(
        args.protoc.resolve(), args.codegen.resolve(), run_dir / "inventory.json")
    summary = build_summary(cases, classifications, duration, args.seed,
                            run_dir, args, selection, corpus_rows, inventory)
    write_json(run_dir / "summary.json", summary)
    write_json(SUMMARY, summary)
    write_findings(summary, run_dir, args.codegen.resolve(),
                   args.protoc.resolve())

    counts = summary["counts"]
    print()
    print("cases:            %d" % summary["cases"])
    print("pass:             %d" % counts["pass"])
    print("known instances:  %d %s" % (counts["known-root"],
                                        summary["known_by_root"]))
    print("allowed stricter: %d %s" % (counts["allowed-stricter"],
                                        [item["policy"] for item in
                                         summary["allowed_stricter_detail"]]))
    print("new roots:        %d" % counts["new-root"])
    print("false positives:  %d" % counts["false-positive"])
    print("generator errors: %d" % counts["generator-error"])
    print("runner errors:    %d" % counts["runner-error"])
    print("corpus rows:      %d" % len(corpus_rows))
    print("crashes/timeouts: %d / %d" % (len(summary["crashes"]),
                                        len(summary["timeouts"])))
    print("duration:         %.1fs" % summary["duration_s"])
    print("summary:          %s" % SUMMARY)
    if counts["new-root"]:
        print("findings:         %s" % PILOT_FINDINGS)
    if counts["new-root"] or counts["false-positive"] or counts["runner-error"] \
            or counts["generator-error"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
