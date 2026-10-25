"""Differential harness pilot (step B2, milestone M1).

Runs the seed corpus against the pinned protoc oracle and the codegen binary:

* P1 — acceptance parity, source path and descriptor path;
* P2 — SHA-256 identity of the header generated from source and from the
  protoc-written descriptor set.

Each case gets its own directory below ``runs/<run-id>/``; both tools are
started with an argv list, a timeout and a fixed environment.  The run writes a
machine-readable ``summary.json`` next to the per-case ``result.json`` files.

Usage:
    python tests/codegen/harness/run.py --cases 200 --seed 1
    python tests/codegen/harness/run.py --cases 0 --only CR-01
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import platform
import random
import subprocess
import sys
import time

import compare
import fds
import runner

HERE = pathlib.Path(__file__).resolve().parent
REPO = runner.repo_root(HERE)
DEFAULT_CORPUS = HERE / "corpus" / "rows"
DEFAULT_PROTOC = pathlib.Path(os.environ.get("EASYPB_PROTOC", "protoc"))
DEFAULT_CODEGEN = pathlib.Path(os.environ.get("EASYPB_CODEGEN", "codegen"))


def load_rows(corpus_dir: pathlib.Path) -> list[dict]:
    rows = []
    for path in sorted(corpus_dir.glob("*.expect.json")):
        with open(path, encoding="utf-8") as handle:
            row = json.load(handle)
        row["_sidecar"] = path.name
        rows.append(row)
    if not rows:
        raise SystemExit("no corpus rows under %s" % corpus_dir)
    return rows


def select_rows(
    rows: list[dict], cases: int, seed: int
) -> tuple[list[dict], list[dict], str]:
    """Return (to-run, not-comparable, selection note)."""
    not_comparable = [row for row in rows if row["kind"] == "not-comparable"]
    comparable = [row for row in rows if row["kind"] != "not-comparable"]
    if cases <= 0 or len(comparable) <= cases:
        selected = comparable
        note = "all %d comparable rows" % len(comparable)
    else:
        mandatory = [row for row in comparable if row.get("mandatory")]
        rest = [row for row in comparable if not row.get("mandatory")]
        if len(mandatory) >= cases:
            selected = mandatory
            note = (
                "all %d mandatory rows; --cases %d is below the mandatory count"
                % (len(mandatory), cases)
            )
        else:
            rng = random.Random(seed)
            sampled = rng.sample(rest, cases - len(mandatory))
            selected = mandatory + sampled
            note = (
                "%d mandatory rows + %d sampled from %d optional rows (seed %d)"
                % (len(mandatory), len(sampled), len(rest), seed)
            )
    selected = sorted(selected, key=lambda row: row["id"])
    not_comparable = sorted(not_comparable, key=lambda row: row["id"])
    return selected, not_comparable, note


def abs_input(row: dict) -> pathlib.Path:
    return REPO / row["input"]


def include_roots(row: dict) -> list[pathlib.Path]:
    return [REPO / path for path in row["include_roots"]]


def logical_name(row: dict, input_path: pathlib.Path) -> str:
    for root in include_roots(row):
        try:
            return input_path.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            continue
    return input_path.name


def source_commands(
    row: dict, codegen: pathlib.Path, protoc: pathlib.Path,
    case_dir: pathlib.Path, out_codegen: pathlib.Path, out_protoc: pathlib.Path,
) -> tuple[list[str], list[str], str]:
    input_path = abs_input(row)
    roots = include_roots(row)
    logical = logical_name(row, input_path)
    include_args: list[str] = []
    for root in roots:
        include_args += ["-I", str(root)]
    codegen_argv = [
        str(codegen), *include_args, "--out-dir", str(out_codegen), str(input_path),
    ]
    protoc_argv = [
        str(protoc), *include_args,
        "--cpp_out=" + str(out_protoc),
        "--descriptor_set_out=" + str(case_dir / "source.pbs"),
        "--include_imports",
        str(input_path),
    ]
    return codegen_argv, protoc_argv, logical


def descriptor_commands(
    row: dict, codegen: pathlib.Path, protoc: pathlib.Path, case_dir: pathlib.Path,
    out_codegen: pathlib.Path, out_protoc: pathlib.Path,
) -> tuple[list[str], list[str], str]:
    input_path = abs_input(row)
    target = row.get("target")
    if not target:
        files = fds.file_descriptors(input_path.read_bytes())
        target = fds.select_target(files)
    if not target:
        raise ValueError("descriptor set has no target")
    codegen_argv = [
        str(codegen), "--descriptor-set", "--target", target,
        "--out-dir", str(out_codegen), str(input_path),
    ]
    protoc_argv = [
        str(protoc), "--descriptor_set_in=" + str(input_path),
        "--cpp_out=" + str(out_protoc), target,
    ]
    return codegen_argv, protoc_argv, target


def header_abs(out_dir: pathlib.Path, verdict: compare.Verdict) -> str | None:
    if not verdict.artifact.path:
        return None
    return str(out_dir / verdict.artifact.path)


def run_case(
    row: dict, run_dir: pathlib.Path, codegen: pathlib.Path, protoc: pathlib.Path,
    default_timeout: float, env_overrides: dict[str, str] | None = None,
) -> dict:
    case_dir = run_dir / row["id"]
    out_codegen = case_dir / "codegen"
    out_protoc = case_dir / "protoc"
    out_descriptor = case_dir / "codegen-from-pbs"
    for directory in (case_dir, out_codegen, out_protoc):
        directory.mkdir(parents=True, exist_ok=True)
    timeout_s = float(row.get("timeout_s") or default_timeout)

    result: dict = {
        "id": row["id"],
        "source": row["source"],
        "kind": row["kind"],
        "properties": row.get("properties", []),
        "input": row.get("input"),
        "root": row.get("root"),
        "cr": row.get("cr"),
        "exception": row.get("exception"),
        "expected": {"p1": row.get("p1_expect"), "p2": row.get("p2_expect")},
        "note": row.get("note"),
        "timeout_s": timeout_s,
    }

    try:
        if row["kind"] == "source":
            cg_argv, pt_argv, logical = source_commands(
                row, codegen, protoc, case_dir, out_codegen, out_protoc
            )
        else:
            cg_argv, pt_argv, logical = descriptor_commands(
                row, codegen, protoc, case_dir, out_codegen, out_protoc
            )
    except (OSError, ValueError, fds.WireError) as error:
        result["runner_error"] = "setup: %s" % error
        result["p1"] = {"property": "P1", "classification": "runner-error",
                        "match": None, "error": str(error)}
        result["p2"] = {"property": "P2", "classification": "n/a", "match": None}
        return result

    codegen_result = runner.execute(cg_argv, str(case_dir), timeout_s,
                                    out_codegen, env_overrides)
    protoc_result = runner.execute(pt_argv, str(case_dir), timeout_s,
                                   out_protoc, env_overrides)
    runner.write_capture(str(case_dir), "codegen", codegen_result)
    runner.write_capture(str(case_dir), "protoc", protoc_result)
    result["logical_name"] = logical
    result["codegen"] = codegen_result.as_dict()
    result["protoc"] = protoc_result.as_dict()

    cg_verdict = compare.codegen_verdict(codegen_result, logical)
    pt_verdict = compare.protoc_verdict(protoc_result, logical)
    result["p1"] = compare.check_p1(
        row, cg_verdict, pt_verdict, codegen_result.stderr_text()
    )
    result["p1"]["id"] = row["id"]

    p2: dict = {"property": "P2", "applicable": False, "classification": "n/a",
                "match": None}
    if row["kind"] == "source":
        if cg_verdict.accept and pt_verdict.accept:
            pbs_path = case_dir / "source.pbs"
            if not pbs_path.is_file() or pbs_path.stat().st_size == 0:
                p2 = {
                    "property": "P2", "applicable": True, "match": False,
                    "classification": compare.classify(row, "P2", True, False),
                    "root": row.get("root"), "exception": row.get("exception"),
                    "reason": "protoc wrote no descriptor set",
                }
            else:
                out_descriptor.mkdir(parents=True, exist_ok=True)
                desc_argv = [
                    str(codegen), "--descriptor-set", "--target", logical,
                    "--out-dir", str(out_descriptor), str(pbs_path),
                ]
                desc_result = runner.execute(
                    desc_argv, str(case_dir), timeout_s, out_descriptor,
                    env_overrides,
                )
                runner.write_capture(str(case_dir), "codegen-from-pbs", desc_result)
                result["codegen_from_pbs"] = desc_result.as_dict()
                desc_verdict = compare.codegen_verdict(desc_result, logical)
                p2 = compare.check_p2(
                    row, cg_verdict, desc_verdict,
                    header_abs(out_codegen, cg_verdict),
                    header_abs(out_descriptor, desc_verdict),
                )
                if not desc_verdict.accept and p2.get("applicable"):
                    p2["descriptor_rc"] = desc_result.rc
                    p2["descriptor_stderr"] = desc_result.stderr_text(800)
    result["p2"] = p2
    result["p2"]["id"] = row["id"]

    crashes = []
    if codegen_result.crashed:
        crashes.append("codegen: %s (rc=0x%08X)" % (
            codegen_result.crash_name, codegen_result.rc))
    if protoc_result.crashed:
        crashes.append("protoc: %s (rc=0x%08X)" % (
            protoc_result.crash_name, protoc_result.rc))
    if crashes:
        result["crashes"] = crashes
    if codegen_result.error:
        result["runner_error"] = codegen_result.error
    if protoc_result.error:
        result["runner_error"] = (
            (result.get("runner_error") + "; ") if result.get("runner_error") else ""
        ) + protoc_result.error
    return result


def tool_version(protoc: pathlib.Path, codegen: pathlib.Path) -> dict:
    def capture(argv: list[str]) -> dict:
        try:
            done = subprocess.run(argv, capture_output=True, timeout=20, shell=False)
            return {
                "command": argv,
                "rc": done.returncode,
                "stdout": done.stdout.decode("utf-8", "replace").strip(),
                "stderr": done.stderr.decode("utf-8", "replace").strip(),
            }
        except (OSError, subprocess.TimeoutExpired) as error:
            return {"command": argv, "error": str(error)}

    return {
        "protoc": capture([str(protoc), "--version"]),
        "codegen": capture([str(codegen), "--help"]),
        "protoc_path": str(protoc),
        "codegen_path": str(codegen),
    }


def inventory_counters(protoc: pathlib.Path, codegen: pathlib.Path,
                       target: pathlib.Path) -> dict:
    script = REPO / "build/verification/check-inventory.py"
    if not script.is_file():
        return {"error": "check-inventory.py not found"}
    try:
        done = subprocess.run(
            [sys.executable, str(script), "--codegen", str(codegen),
             "--json", str(target)],
            capture_output=True, timeout=120, shell=False,
        )
        if done.returncode != 0:
            return {"error": done.stderr.decode("utf-8", "replace").strip()[:500]}
        return json.loads(target.read_text(encoding="utf-8"))
    except (OSError, subprocess.TimeoutExpired, ValueError) as error:
        return {"error": str(error)}


def summarise(
    results: list[dict], rows: list[dict], not_comparable: list[dict],
    run_dir: pathlib.Path, duration_s: float, args, selection_note: str,
) -> dict:
    p1_results = [item["p1"] for item in results if item.get("p1")]
    p2_results = [item["p2"] for item in results if item.get("p2")]
    p1_mismatch = [item for item in p1_results if item.get("match") is False]
    p2_applicable = [item for item in p2_results if item.get("applicable")]
    p2_mismatch = [item for item in p2_applicable if item.get("match") is False]

    def count(items: list[dict], classification: str) -> int:
        return sum(1 for item in items if item.get("classification") == classification)

    def ids(items: list[dict]) -> list[str]:
        return sorted(item["id"] for item in items)

    row_by_id = {row["id"]: row for row in rows}
    new_cases = sorted(set(
        item["id"] for item in p1_mismatch + p2_mismatch
        if item.get("classification") == "new"
    ))
    false_positive_cases = sorted(set(
        item["id"] for item in p1_mismatch + p2_mismatch
        if item.get("classification") == "false-positive"
    ))
    known_cases = sorted(set(
        item["id"] for item in p1_mismatch + p2_mismatch
        if item.get("classification") in ("known", "exception")
    ))
    allowed_cases = sorted(set(
        item["id"] for item in p1_mismatch
        if item.get("classification") == "allowed-stricter"
    ))

    crashes = []
    timeouts = []
    runner_errors = []
    for item in results:
        if item.get("crashes"):
            crashes.append({"id": item["id"], "crashes": item["crashes"]})
        for tool in ("codegen", "protoc", "codegen_from_pbs"):
            if item.get(tool, {}).get("timed_out"):
                timeouts.append({"id": item["id"], "tool": tool})
        if item.get("runner_error"):
            runner_errors.append({"id": item["id"], "error": item["runner_error"]})

    accepted_source = sum(
        1 for item in results
        if item.get("p1", {}).get("codegen_accept") and item["kind"] == "source"
    )
    accepted_descriptor = sum(
        1 for item in results
        if item.get("p1", {}).get("codegen_accept") and item["kind"] == "descriptor"
    )

    summary = {
        "schema": 1,
        "run_id": run_dir.name,
        "started_at": args._started_at,
        "finished_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "duration_s": round(duration_s, 2),
        "command": args._command,
        "seed": args.seed,
        "cases_requested": args.cases,
        "cases_available": len([row for row in rows if row["kind"] != "not-comparable"]),
        "cases_run": len(results),
        "selection": selection_note,
        "not_comparable": [
            {"id": row["id"], "cr": row.get("cr"), "reason": row.get("reason")}
            for row in not_comparable
        ],
        "verdict_mismatches": len(p1_mismatch),
        "p2_mismatches": len(p2_mismatch),
        "compile_failures": None,
        "compile_failures_note": "P3 is outside M1 (B2 milestone M2)",
        "new_roots": len(new_cases),
        "new_root_cases": new_cases,
        "known_instances": len(known_cases),
        "known_cases": known_cases,
        "allowed_stricter": len(allowed_cases),
        "allowed_stricter_cases": allowed_cases,
        "false_positives": len(false_positive_cases),
        "false_positive_cases": false_positive_cases,
        "crashes": crashes,
        "timeouts": timeouts,
        "runner_errors": runner_errors,
        "properties": {
            "P1": {
                "checks": len(p1_results),
                "matches": len(p1_results) - len(p1_mismatch),
                "mismatches": len(p1_mismatch),
                "known": count(p1_mismatch, "known") + count(p1_mismatch, "exception"),
                "allowed_stricter": count(p1_mismatch, "allowed-stricter"),
                "new": count(p1_mismatch, "new"),
                "false_positives": count(p1_mismatch, "false-positive"),
                "mismatch_ids": ids(p1_mismatch),
            },
            "P2": {
                "applicable": len(p2_applicable),
                "matches": len(p2_applicable) - len(p2_mismatch),
                "mismatches": len(p2_mismatch),
                "known": count(p2_mismatch, "known") + count(p2_mismatch, "exception"),
                "allowed_stricter": 0,
                "new": count(p2_mismatch, "new"),
                "false_positives": count(p2_mismatch, "false-positive"),
                "mismatch_ids": ids(p2_mismatch),
            },
        },
        "coverage": {
            "source_cases": sum(1 for item in results if item["kind"] == "source"),
            "descriptor_cases": sum(1 for item in results
                                    if item["kind"] == "descriptor"),
            "accepted_source_cases": accepted_source,
            "accepted_descriptor_cases": accepted_descriptor,
            "inventory": None,
            "note": "M1 seed corpus only; generated-case coverage is M2",
        },
        "tools": tool_version(
            pathlib.Path(args.protoc).resolve(), pathlib.Path(args.codegen).resolve()
        ),
        "environment": {
            "python": sys.version.split()[0],
            "platform": platform.platform(),
        },
        "results_file": str((run_dir / "results.json").resolve()),
        "case_results": [
            {"id": item["id"], "kind": item["kind"],
             "p1": item.get("p1", {}).get("classification"),
             "p2": item.get("p2", {}).get("classification")}
            for item in results
        ],
        "rows_with_root": len([row for row in rows if row.get("root")]),
        "row_ids": sorted(row_by_id),
    }
    return summary


def write_json(path: pathlib.Path, payload: dict) -> None:
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=False, ensure_ascii=True) + "\n",
        encoding="utf-8", newline="\n",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cases", type=int, default=200,
                        help="maximum comparable cases; 0 or more than the corpus runs all")
    parser.add_argument("--seed", type=int, default=1,
                        help="sampling seed when --cases is below the corpus size")
    parser.add_argument("--corpus", type=pathlib.Path, default=DEFAULT_CORPUS)
    parser.add_argument("--protoc", type=pathlib.Path, default=DEFAULT_PROTOC)
    parser.add_argument("--codegen", type=pathlib.Path, default=DEFAULT_CODEGEN)
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="default per-process timeout in seconds")
    parser.add_argument("--only", action="append", default=None,
                        help="run only rows whose id contains this text; repeatable")
    parser.add_argument("--out", type=pathlib.Path, default=None)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()
    args.protoc = runner.resolve_tool(args.protoc)
    args.codegen = runner.resolve_tool(args.codegen)
    args.corpus = args.corpus.resolve()

    started = time.monotonic()
    args._started_at = dt.datetime.now().astimezone().isoformat(timespec="seconds")
    args._command = [sys.executable] + sys.argv
    if not args.protoc.is_file():
        print("protoc not found: %s" % args.protoc, file=sys.stderr)
        return 2
    if not args.codegen.is_file():
        print("codegen not found: %s" % args.codegen, file=sys.stderr)
        return 2

    rows = load_rows(args.corpus)
    if args.only:
        filters = [text.lower() for text in args.only]
        rows = [row for row in rows
                if any(text in row["id"].lower() for text in filters)]
        if not rows:
            print("no row matched --only", file=sys.stderr)
            return 2
    selected, not_comparable, selection_note = select_rows(rows, args.cases, args.seed)

    run_id = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = (args.out or (HERE / "runs" / run_id))
    run_dir = run_dir.resolve()
    run_dir.mkdir(parents=True, exist_ok=True)
    print("run: %s" % run_dir)
    print("selection: %s" % selection_note)
    print("tools: protoc=%s codegen=%s" % (args.protoc, args.codegen))

    results = []
    for index, row in enumerate(selected, start=1):
        case_started = time.monotonic()
        try:
            item = run_case(row, run_dir, args.codegen.resolve(),
                            args.protoc.resolve(), args.timeout)
        except Exception as error:  # keep the run going; record the harness error
            item = {
                "id": row["id"], "kind": row["kind"], "source": row["source"],
                "runner_error": "%s: %s" % (type(error).__name__, error),
                "p1": {"classification": "runner-error", "match": None},
                "p2": {"classification": "n/a", "match": None},
            }
        item["duration_s"] = round(time.monotonic() - case_started, 3)
        results.append(item)
        case_file = run_dir / row["id"] / "result.json"
        case_file.parent.mkdir(parents=True, exist_ok=True)
        write_json(case_file, item)
        if not args.quiet:
            p1 = item.get("p1", {})
            p2 = item.get("p2", {})
            print("[%3d/%3d] %-64s P1=%-14s P2=%-14s %6.2fs" % (
                index, len(selected), row["id"],
                p1.get("classification", "-"), p2.get("classification", "-"),
                item["duration_s"],
            ))

    duration = time.monotonic() - started
    summary = summarise(results, rows, not_comparable, run_dir, duration, args,
                        selection_note)
    summary["coverage"]["inventory"] = inventory_counters(
        args.protoc.resolve(), args.codegen.resolve(), run_dir / "inventory.json"
    )
    summary["coverage"]["inventory_note"] = (
        "B1 register denominators; the seed corpus is not a generated-case "
        "coverage measurement"
    )
    write_json(run_dir / "summary.json", summary)
    write_json(run_dir / "results.json", {"results": results})
    write_json(HERE / "summary.json", summary)

    print()
    print("cases:            %d run, %d available, %d not comparable" % (
        summary["cases_run"], summary["cases_available"],
        len(summary["not_comparable"])))
    print("verdict mismatches: %d (P1) + %d (P2)" % (
        summary["verdict_mismatches"], summary["p2_mismatches"]))
    print("known instances:  %d" % summary["known_instances"])
    print("allowed stricter: %d %s" % (
        summary["allowed_stricter"], summary["allowed_stricter_cases"] or ""))
    print("new roots:        %d %s" % (
        summary["new_roots"], summary["new_root_cases"] or ""))
    print("false positives:  %d %s" % (
        summary["false_positives"], summary["false_positive_cases"] or ""))
    print("crashes/timeouts: %d / %d" % (
        len(summary["crashes"]), len(summary["timeouts"])))
    print("duration:         %.1fs" % summary["duration_s"])
    print("summary:          %s" % (run_dir / "summary.json"))
    print("latest copy:      %s" % (HERE / "summary.json"))

    if summary["runner_errors"] or summary["new_roots"] or summary["false_positives"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
