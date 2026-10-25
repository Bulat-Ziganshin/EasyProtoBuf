"""P7 thin-vs-full check for the differential harness (step B2b).

The property (``property-catalogue.md`` P7, owner decision W-68): for the same
target and options, the compatibility-mode header generated from a thin
descriptor set (``protoc`` without ``--include_imports``) is byte-identical to
the header generated from the complete closure and from source.  The semantic
half is a runtime probe: a message whose field names an external enum must not
carry an indeterminate value when the dependency definition is unavailable.

The fixtures are CR-08's: ``emitter-names/thin.pbs`` (``b.proto`` alone),
``emitter-names/full.pbs`` (``a.proto`` + ``b.proto``), ``emitter-names/ext``
(the source triple) and ``emitter-names/thin-read.cpp`` (the placement-new
probe).

The pinned pre-fix binary violates P7 exactly as CR-08 describes, so a
divergence whose signature is "thin differs from full/source and the thin probe
reads an indeterminate enum" is recorded as ``known-cr-08``.  Any other
divergence is unexplained and exits 1.  B17 re-runs this after B8.

Usage:
    python tests/codegen/harness/thin.py
    python tests/codegen/harness/thin.py --keep
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import platform
import re
import subprocess
import sys

import runner

HERE = pathlib.Path(__file__).resolve().parent
REPO = runner.repo_root(HERE)
REVIEW = HERE / "corpus" / "review" / "emitter-names"
DEFAULT_GXX = pathlib.Path(os.environ.get("EASYPB_GXX", "g++"))
LOGICAL = "b.proto"


def write_json(path: pathlib.Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8", newline="\n",
    )


def run(argv: list[str], cwd: pathlib.Path, timeout: float = 60.0) -> dict:
    done = subprocess.run(argv, cwd=str(cwd), capture_output=True,
                          shell=False, timeout=timeout)
    return {
        "argv": argv,
        "rc": done.returncode,
        "stdout": done.stdout.decode("utf-8", "replace"),
        "stderr": done.stderr.decode("utf-8", "replace"),
    }


def generate(codegen: pathlib.Path, work: pathlib.Path) -> dict:
    """Generate the thin, full and source headers into separate directories."""
    thin_dir = work / "thin"
    full_dir = work / "full"
    source_dir = work / "source"
    for directory in (thin_dir, full_dir, source_dir):
        directory.mkdir(parents=True, exist_ok=True)

    commands = {
        "thin": [
            str(codegen), "--descriptor-set", "--out-dir", str(thin_dir),
            str(REVIEW / "thin.pbs"),
        ],
        "full": [
            str(codegen), "--descriptor-set", "--target", LOGICAL,
            "--out-dir", str(full_dir), str(REVIEW / "full.pbs"),
        ],
        "source": [
            str(codegen), "-I", str(REVIEW / "ext"),
            "--out-dir", str(source_dir), str(REVIEW / "ext" / LOGICAL),
        ],
    }
    results = {}
    for name, argv in commands.items():
        outcome = run(argv, work)
        header = work / name / (LOGICAL[: -len(".proto")] + ".pb.hpp")
        results[name] = {
            **outcome,
            "header": str(header) if header.is_file() else None,
            "sha256": runner.sha256_file(header) if header.is_file() else None,
        }
    # The dependency header the thin header includes is generated from source;
    # that is the documented contract ("generate the dependencies as well").
    dependency = run([
        str(codegen), "-I", str(REVIEW / "ext"),
        "--out-dir", str(source_dir), str(REVIEW / "ext" / "a.proto"),
    ], work)
    results["dependency"] = {
        **dependency,
        "header": str(source_dir / "a.pb.hpp")
        if (source_dir / "a.pb.hpp").is_file() else None,
    }
    for name in ("thin", "full"):
        if results["dependency"]["header"]:
            (work / name / "a.pb.hpp").write_bytes(
                pathlib.Path(results["dependency"]["header"]).read_bytes())
    return results


def build_probe(gxx: pathlib.Path, directory: pathlib.Path) -> dict:
    executable = directory / "probe.exe"
    compile_result = run([
        str(gxx), "-std=c++11", "-pedantic-errors", "-I", str(REPO / "include"),
        "-I", str(directory), str(REVIEW / "thin-read.cpp"), "-o",
        str(executable),
    ], directory, timeout=120.0)
    if compile_result["rc"] != 0:
        return {"compiled": False, **compile_result}
    started = run([str(executable)], directory, timeout=30.0)
    values: dict = {"compiled": True, "run": started}
    for key in ("e", "has_e"):
        match = re.search(r"\b%s=(-?\d+)" % key, started["stdout"])
        values[key] = int(match.group(1)) if match else None
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codegen", type=pathlib.Path,
                        default=pathlib.Path(os.environ.get("EASYPB_CODEGEN", "codegen")))
    parser.add_argument("--gxx", type=pathlib.Path, default=DEFAULT_GXX)
    parser.add_argument("--out", type=pathlib.Path, default=None)
    parser.add_argument("--keep", action="store_true",
                        help="keep the generated work directory")
    args = parser.parse_args()
    args.codegen = runner.resolve_tool(args.codegen)
    args.gxx = runner.resolve_tool(args.gxx)

    if not args.codegen.is_file():
        print("codegen not found: %s" % args.codegen, file=sys.stderr)
        return 2
    if not args.gxx.is_file():
        print("g++ not found: %s" % args.gxx, file=sys.stderr)
        return 2

    run_id = dt.datetime.now().strftime("p7-%Y%m%d-%H%M%S")
    work = (args.out or (HERE / "runs" / run_id / "work")).resolve()
    work.mkdir(parents=True, exist_ok=True)
    print("work: %s" % work)

    generated = generate(args.codegen.resolve(), work)
    print("thin:   rc=%d sha256=%s" % (
        generated["thin"]["rc"], generated["thin"]["sha256"]))
    print("full:   rc=%d sha256=%s" % (
        generated["full"]["rc"], generated["full"]["sha256"]))
    print("source: rc=%d sha256=%s" % (
        generated["source"]["rc"], generated["source"]["sha256"]))

    probes = {}
    for name in ("thin", "full"):
        probes[name] = build_probe(args.gxx.resolve(), work / name)
        print("%-6s probe: %s" % (name, {
            key: probes[name].get(key) for key in ("compiled", "e", "has_e")}))

    pairs = {
        "thin_vs_full": generated["thin"]["sha256"] == generated["full"]["sha256"],
        "thin_vs_source": generated["thin"]["sha256"] == generated["source"]["sha256"],
        "full_vs_source": generated["full"]["sha256"] == generated["source"]["sha256"],
    }
    generated_ok = all(generated[name]["rc"] == 0 for name in ("thin", "full", "source"))
    thin_probe_zero = probes["thin"].get("e") == 0 and probes["thin"].get("has_e") == 0
    full_probe_zero = probes["full"].get("e") == 0 and probes["full"].get("has_e") == 0

    # CR-08's signature: the thin header differs from both complete headers and
    # the thin probe reads an indeterminate enum; the complete header is clean.
    known_cr08 = (
        generated_ok
        and not pairs["thin_vs_full"]
        and not pairs["thin_vs_source"]
        and pairs["full_vs_source"]
        and full_probe_zero
        and not thin_probe_zero
        and probes["thin"].get("e") not in (None, 0)
    )
    unexplained = []
    if not generated_ok:
        unexplained.append("generation failed: %s" % {
            name: generated[name]["rc"] for name in ("thin", "full", "source")})
    if not pairs["full_vs_source"]:
        unexplained.append("full and source headers differ")
    if not pairs["thin_vs_full"] and not known_cr08:
        unexplained.append("thin differs from full with no CR-08 signature")
    if not thin_probe_zero and not known_cr08:
        unexplained.append("thin runtime probe is not clean and not CR-08")
    if not full_probe_zero:
        unexplained.append("full runtime probe is not clean")
    if not probes["thin"].get("compiled") or not probes["full"].get("compiled"):
        unexplained.append("runtime probe did not compile")

    summary = {
        "schema": 1,
        "step": "B2b",
        "property": "P7",
        "run_id": run_id,
        "started_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "command": [sys.executable] + sys.argv,
        "fixtures": {
            "thin": str(REVIEW / "thin.pbs"),
            "full": str(REVIEW / "full.pbs"),
            "source": str(REVIEW / "ext" / LOGICAL),
            "probe": str(REVIEW / "thin-read.cpp"),
        },
        "generated": {
            name: {key: generated[name].get(key)
                   for key in ("rc", "stderr", "header", "sha256")}
            for name in ("thin", "full", "source")
        },
        "pairs": pairs,
        "probe": probes,
        "verdict": "known-cr-08" if known_cr08 else (
            "unexplained" if unexplained else "pass"),
        "unexplained": unexplained,
        "note": ("W-68: the thin-set header is byte-identical to the "
                 "complete-closure header; a pre-fix divergence is the CR-08 "
                 "baseline and B17 owns the final verdict"),
        "tools": {
            "codegen": str(args.codegen.resolve()),
            "codegen_sha256": runner.sha256_file(args.codegen.resolve()),
            "gxx": str(args.gxx.resolve()),
            "environment": platform.platform(),
        },
    }
    run_dir = work.parent
    write_json(run_dir / "p7-summary.json", summary)
    write_json(HERE / "p7-summary.json", summary)

    print("pairs:  %s" % pairs)
    print("verdict: %s" % summary["verdict"])
    for item in unexplained:
        print("  UNEXPLAINED %s" % item)
    if not args.keep:
        pass
    print("summary: %s" % (run_dir / "p7-summary.json"))
    return 1 if unexplained else 0


if __name__ == "__main__":
    sys.exit(main())
