"""Hand-built descriptor-set plan for the differential harness (step B2b).

``mutate.py`` starts from a set protoc wrote; these fifteen shapes are written
by ``wire.py`` and are ones protoc would never produce (a duplicate field
number, an absent kind, an editions file, a repeated public index).  Each shape
carries the triage root or P1 exception that the owner decision assigns; this
driver builds it, runs both tools through the normal runner, and checks that
the observed verdict is the predicted one.

The plan is fixed and reproducible: the bytes are built by the code, not
sampled.  A shape that changes class, or that produces a divergence no root or
exception explains, is a new finding and exits 1.

Usage:
    python tests/codegen/harness/handbuilt.py --dry-run
    python tests/codegen/harness/handbuilt.py
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import sys

import mutate
import run
import runner
import seed

HERE = pathlib.Path(__file__).resolve().parent
REPO = runner.repo_root(HERE)
GENERATED = HERE / "generated" / "handbuilt"
CORPUS_GENERATED = HERE / "corpus" / "generated"
CORPUS_ROWS = HERE / "corpus" / "rows"
SUMMARY = HERE / "handbuilt-summary.json"

# Diagnostic markers that identify the owner decision behind an exception.
EXCEPTION_MARKERS = {
    "E4": ("declares no type", "does not support group",
           "declares an unknown type", "invalid field type",
           "unknown field type", "field kind"),
    "E8": ("editions syntax is not supported", "declares edition"),
    "E9": ("duplicate public dependency index", "both public and weak",
           "public dependency"),
    "E10": ("Malformed map entry",),
}


def relative(path: pathlib.Path) -> str:
    return path.resolve().relative_to(REPO).as_posix()


def build_row(shape: str, mutation: mutate.Mutation, path: pathlib.Path) -> dict:
    return {
        "schema": 1,
        "id": shape,
        "source": "handbuilt",
        "kind": "descriptor",
        "properties": ["P1"],
        "input": relative(path),
        "include_roots": [],
        "target": mutation.target,
        "cr": None,
        "root": None,
        "exception": None,
        "p1_expect": "n/a",
        "p2_expect": "n/a",
        "timeout_s": 10.0,
        "mandatory": True,
        "reason": None,
        "note": "hand-built descriptor set: %s" % mutation.detail,
        "handbuilt": {
            "shape": shape,
            "descriptor_path": mutation.descriptor_path,
            "detail": mutation.detail,
        },
    }


def classify(shape: str, expected: str, authority: str | None,
             direction: str | None, item: dict) -> dict:
    p1 = item.get("p1") or {}
    codegen_accept = p1.get("codegen_accept")
    protoc_accept = p1.get("protoc_accept")
    stderr = ((item.get("codegen") or {}).get("stderr") or "")
    if p1.get("match") is True:
        observed = "parity"
        detail = "both tools agree"
    elif codegen_accept is False and protoc_accept is True:
        marker = next((marker for marker in EXCEPTION_MARKERS.get(authority, ())
                       if marker in stderr), None)
        if expected == "exception" and marker:
            observed = "exception"
            detail = marker
        elif expected == "known-root" and direction == "stricter":
            observed = "known-root"
            detail = "codegen refuses, protoc accepts (the stricter direction)"
        else:
            observed = "stricter-unexplained"
            detail = marker or "no exception marker in the refusal"
    elif codegen_accept is True and protoc_accept is False:
        if expected == "known-root" and direction == "more-permissive":
            observed = "known-root"
            detail = ("codegen accepts, protoc refuses "
                      "(the more-permissive direction)")
        else:
            observed = "more-permissive-unexplained"
            detail = "codegen accepts, protoc refuses"
    else:
        observed = "unknown"
        detail = "neither direction matches a property verdict"
    ok = observed == expected
    return {
        "id": shape,
        "expected": expected,
        "expected_authority": authority,
        "expected_direction": direction,
        "observed": observed,
        "ok": ok,
        "codegen_accept": codegen_accept,
        "protoc_accept": protoc_accept,
        "detail": detail,
        "codegen_stderr": stderr.strip().splitlines()[:1],
        "protoc_stderr": ((item.get("protoc") or {}).get("stderr") or "")
        .strip().splitlines()[:1],
        "p1_classification": p1.get("classification"),
    }


def save_corpus_row(shape: str, expected: str, authority: str,
                    mutation: mutate.Mutation, path: pathlib.Path) -> str:
    directory = CORPUS_GENERATED / shape
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / path.name
    destination.write_bytes(mutation.payload)
    row = build_row(shape, mutation, destination)
    condition = "pass" if expected == "parity" else "divergent"
    row["p1_expect"] = condition
    if expected == "exception":
        row["exception"] = authority
    if expected == "known-root":
        row["root"] = authority
    row["note"] = "hand-built descriptor set, %s: %s" % (
        expected if expected == "parity" else "%s/%s" % (expected, authority),
        mutation.detail)
    CORPUS_ROWS.mkdir(parents=True, exist_ok=True)
    sidecar = CORPUS_ROWS / (shape + ".expect.json")
    sidecar.write_text(seed.serialise(row), encoding="utf-8", newline="\n")
    return relative(sidecar)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protoc", type=pathlib.Path,
                        default=run.DEFAULT_PROTOC)
    parser.add_argument("--codegen", type=pathlib.Path,
                        default=run.DEFAULT_CODEGEN)
    parser.add_argument("--out", type=pathlib.Path, default=None)
    parser.add_argument("--dry-run", action="store_true",
                        help="do not write corpus rows")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()
    if not args.codegen.is_file() or not args.protoc.is_file():
        print("tool missing", file=sys.stderr)
        return 2

    run_id = dt.datetime.now().strftime("handbuilt-%Y%m%d-%H%M%S")
    run_dir = (args.out or (HERE / "runs" / run_id)).resolve()
    run_dir.mkdir(parents=True, exist_ok=True)
    GENERATED.mkdir(parents=True, exist_ok=True)
    print("run:   %s" % run_dir)
    print("shapes: %d" % len(mutate.HAND_BUILT_ORDER))

    outcomes = []
    corpus_rows = []
    for shape in mutate.HAND_BUILT_ORDER:
        _builder, expected, authority, direction = mutate.HAND_BUILT[shape]
        mutation = mutate.hand_built(shape)
        path = GENERATED / (shape + ".pbs")
        path.write_bytes(mutation.payload)
        row = build_row(shape, mutation, path)
        item = run.run_case(row, run_dir, args.codegen.resolve(),
                            args.protoc.resolve(), 10.0)
        outcome = classify(shape, expected, authority, direction, item)
        outcomes.append(outcome)
        if outcome["ok"] and not args.dry_run and expected != "parity":
            corpus_rows.append(save_corpus_row(shape, expected, authority,
                                               mutation, path))
        if not args.quiet:
            print("[%2d/%2d] %-32s expected=%-18s observed=%-18s %s" % (
                len(outcomes), len(mutate.HAND_BUILT_ORDER), shape,
                "%s/%s" % (expected, authority), outcome["observed"],
                "ok" if outcome["ok"] else "MISMATCH"))

    written = [
        {"shape": shape, "id": outcome["id"],
         "expected": outcome["expected"],
         "authority": outcome["expected_authority"],
         "codegen_accept": outcome["codegen_accept"],
         "protoc_accept": outcome["protoc_accept"]}
        for shape, outcome in zip(mutate.HAND_BUILT_ORDER, outcomes)
    ]
    failed = [outcome for outcome in outcomes if not outcome["ok"]]
    summary = {
        "schema": 1,
        "step": "B2b",
        "component": "hand-built descriptor sets",
        "run_id": run_id,
        "started_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "command": [sys.executable] + sys.argv,
        "shapes": len(outcomes),
        "ok": len(outcomes) - len(failed),
        "mismatches": len(failed),
        # Any mismatch is unexpected: the plan declares the expected class, so
        # an unexplained direction must not disappear from the summary.
        "new_roots": [outcome["id"] for outcome in failed],
        "detail": outcomes,
        "corpus_rows": corpus_rows,
        "written": written,
        "tools": {
            "codegen": str(args.codegen.resolve()),
            "codegen_sha256": run.runner.sha256_file(args.codegen.resolve()),
            "protoc": str(args.protoc.resolve()),
        },
    }
    run.write_json(run_dir / "handbuilt-summary.json", summary)
    run.write_json(SUMMARY, summary)
    print()
    print("ok:         %d / %d" % (summary["ok"], summary["shapes"]))
    print("mismatches: %d" % summary["mismatches"])
    print("corpus:     %d row(s)" % len(corpus_rows))
    print("summary:    %s" % SUMMARY)
    for outcome in failed:
        print("  MISMATCH %s expected=%s/%s observed=%s: %s" % (
            outcome["id"], outcome["expected"], outcome["expected_authority"],
            outcome["observed"], outcome["detail"]))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
