"""Delta-debugging shrinker for the differential harness (step B2b).

The discovery slice stored the 77 raw generator outputs as corpus rows.  This
module minimizes them while preserving the **verdict**, and writes the smallest
input back into the corpus:

* ``.proto`` source — ddmin over brace-balanced top-level declarations, then
  recursively over the body of every message/enum/service, so a field, an
  option, an import or a whole unused message can disappear;
* ``FileDescriptorSet`` — ddmin over the wire fields of the set, then
  recursively over the known message-typed descriptor fields (file, message,
  field, enum, service, method); a final pass tries the shortest string and a
  zero varint.

The verdict is not just "did something diverge": it pins the P1 classification,
both accept bits, the P2 applicability/match pair, the first diagnostic line of
each tool (normalized) and the P2 first differing value.  A candidate that
turns one divergence into a *different* one is rejected, so shrinking cannot
silently replace the mechanism a corpus row was filed for.

The original input is kept only when the minimized input reproduces a different
verdict (normally impossible: the shrinker only accepts candidates whose
verdict is equal) — the flag is recorded in the sidecar either way.

Usage:
    python tests/codegen/harness/shrink.py --all
    python tests/codegen/harness/shrink.py --row m2-031-defaults
    python tests/codegen/harness/shrink.py --all --budget 300
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

import mutate
import run
import runner
import seed

HERE = pathlib.Path(__file__).resolve().parent
REPO = runner.repo_root(HERE)
CORPUS_ROWS = HERE / "corpus" / "rows"
CORPUS_GENERATED = HERE / "corpus" / "generated"


# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------


def _first_line(text: str | None) -> str:
    if not text:
        return ""
    stripped = text.strip()
    return stripped.splitlines()[0] if stripped else ""


def _normalise(text: str) -> str:
    """Remove the position noise: paths, line/column pairs and path tags."""
    # A diagnostic may name the absolute input path; each shrink candidate
    # lives in its own directory, so only the basename is stable.
    text = re.sub(r"\S*[\\/]([^\\/\s:]+):\d+:\d+:", r"\1:N:N:", text)
    text = re.sub(r":\d+:\d+:", ":N:N:", text)
    text = re.sub(r"\[\d+(?:, \d+)+\]", "[path]", text)
    return re.sub(r"\s+", " ", text).strip()


class Verdict:
    """The invariant a shrink candidate has to preserve."""

    def __init__(self, row: dict, work: pathlib.Path, codegen: pathlib.Path,
                 protoc: pathlib.Path, timeout: float):
        self.row = row
        self.work = work
        self.codegen = codegen
        self.protoc = protoc
        self.timeout = timeout
        self.evaluations = 0
        self.cache: dict[bytes, dict] = {}

    def measure(self, payload: bytes) -> dict:
        if payload in self.cache:
            return self.cache[payload]
        self.evaluations += 1
        case_dir = self.work / ("e%04d" % self.evaluations)
        shutil.rmtree(case_dir, ignore_errors=True)
        case_dir.mkdir(parents=True)
        name = (self.row.get("input") or "case.proto").rsplit("/", 1)[-1]
        input_path = case_dir / name
        input_path.write_bytes(payload)
        candidate = dict(self.row)
        candidate["input"] = str(input_path)
        candidate["include_roots"] = (
            [str(case_dir)] if self.row["kind"] == "source" else [])
        item = run.run_case(candidate, case_dir, self.codegen, self.protoc,
                            self.timeout)
        p1 = item.get("p1") or {}
        p2 = item.get("p2") or {}
        difference = p2.get("first_difference") or {}
        measured = {
            "runner_error": item.get("runner_error"),
            "p1": p1.get("classification"),
            "codegen_accept": p1.get("codegen_accept"),
            "protoc_accept": p1.get("protoc_accept"),
            "p2_applicable": bool(p2.get("applicable")),
            "p2_match": p2.get("match"),
            "codegen_stderr": _normalise(_first_line(
                (item.get("codegen") or {}).get("stderr"))),
            "protoc_stderr": _normalise(_first_line(
                (item.get("protoc") or {}).get("stderr"))),
            "p2_difference": (
                (difference.get("source"), difference.get("descriptor"))
                if difference else None),
            "crashes": tuple(
                tool for tool in ("codegen", "protoc", "codegen_from_pbs")
                if (item.get(tool) or {}).get("crashed")),
            "timeouts": tuple(
                tool for tool in ("codegen", "protoc", "codegen_from_pbs")
                if (item.get(tool) or {}).get("timed_out")),
        }
        self.cache[payload] = measured
        return measured

    def preserved(self, payload: bytes, target: dict) -> bool:
        return self.measure(payload) == target


# ---------------------------------------------------------------------------
# Generic ddmin over index ranges
# ---------------------------------------------------------------------------


def ddmin_indices(count: int, test) -> list[int]:
    """Return the kept indices of ``range(count)``; ``test(list)`` decides.

    Classic delta debugging: split the current sequence into *n* chunks, try to
    remove each chunk, and increase the granularity when no single removal
    works.  ``test`` receives an ordered list of kept indices.
    """
    indices = list(range(count))
    granularity = 2
    while len(indices) >= 2:
        chunk_size = (len(indices) + granularity - 1) // granularity
        reduced = False
        for start in range(0, len(indices), chunk_size):
            chunk = indices[start:start + chunk_size]
            candidate = indices[:start] + indices[start + chunk_size:]
            if candidate and test(candidate):
                indices = candidate
                granularity = max(granularity - 1, 2)
                reduced = True
                break
        if not reduced:
            if granularity >= len(indices):
                break
            granularity = min(granularity * 2, len(indices))
    return indices


# ---------------------------------------------------------------------------
# .proto source shrinking
# ---------------------------------------------------------------------------


def split_proto(text: str) -> list[str]:
    """Split a ``.proto`` source into top-level units.

    A unit is a declaration with balanced braces (``message``, ``enum``,
    ``service``, ``extend``) or a ``;``-terminated statement (``syntax``,
    ``package``, ``import``, ``option``, ``reserved``).  Comments and string
    literals are skipped so a brace inside either does not change the depth.
    """
    units: list[str] = []
    start = 0
    depth = 0
    index = 0
    in_string: str | None = None
    in_line_comment = False
    in_block_comment = False
    while index < len(text):
        char = text[index]
        nxt = text[index + 1] if index + 1 < len(text) else ""
        if in_line_comment:
            if char == "\n":
                in_line_comment = False
        elif in_block_comment:
            if char == "*" and nxt == "/":
                in_block_comment = False
                index += 1
        elif in_string:
            if char == "\\":
                index += 1
            elif char == in_string:
                in_string = None
        elif char == "/" and nxt == "/":
            in_line_comment = True
            index += 1
        elif char == "/" and nxt == "*":
            in_block_comment = True
            index += 1
        elif char in "\"'":
            in_string = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                # A block declaration ends here; an aggregate option value is
                # followed by `;` and the whole statement is one unit.
                lookahead = index + 1
                while lookahead < len(text) and text[lookahead].isspace():
                    lookahead += 1
                if lookahead < len(text) and text[lookahead] == ";":
                    units.append(text[start:lookahead + 1].strip())
                    index = lookahead
                    start = index + 1
                else:
                    units.append(text[start:index + 1].strip())
                    start = index + 1
        elif char == ";" and depth == 0:
            units.append(text[start:index + 1].strip())
            start = index + 1
        index += 1
    tail = text[start:].strip()
    if tail:
        units.append(tail)
    return [unit for unit in units if unit]


def split_body(unit: str) -> tuple[str, list[str], str] | None:
    """Split ``head { body }`` into its head, body units and tail."""
    brace = unit.find("{")
    if brace < 0 or not unit.rstrip().endswith("}"):
        return None
    close = unit.rfind("}")
    head = unit[: brace + 1]
    body = unit[brace + 1:close]
    tail = unit[close:]
    return head, split_proto(body), tail


def shrink_proto(text: str, preserved, budget: int) -> str:
    units = split_proto(text)

    def joined(chosen: list[int]) -> str:
        return "\n".join(units[index] for index in chosen) + "\n"

    def test(chosen: list[int]) -> bool:
        return preserved(joined(chosen).encode("utf-8"))

    kept = ddmin_indices(len(units), test)
    units = [units[index] for index in kept]

    # Recurse into every declaration body: a message body is itself a list of
    # statements, so an unused field or nested type can disappear.
    for position, unit in enumerate(units):
        split = split_body(unit)
        if not split:
            continue
        head, body_units, tail = split

        def joined_body(chosen: list[int]) -> bytes:
            candidate = list(units)
            candidate[position] = head + "\n" + "\n".join(
                body_units[index] for index in chosen) + "\n" + tail
            return ("\n".join(candidate) + "\n").encode("utf-8")

        def test_body(chosen: list[int]) -> bool:
            return preserved(joined_body(chosen))

        body_kept = ddmin_indices(len(body_units), test_body)
        units[position] = head + "\n" + "\n".join(
            body_units[index] for index in body_kept) + "\n" + tail
    return "\n".join(units) + "\n"


# ---------------------------------------------------------------------------
# FileDescriptorSet shrinking
# ---------------------------------------------------------------------------

CHILD_KINDS = {
    "fds": {1: "file"},
    "file": {4: "message", 5: "enum", 6: "service", 7: "field"},
    "message": {2: "field", 3: "message", 4: "enum", 5: "extension-range",
                6: "field", 7: "message-options", 8: "oneof",
                9: "reserved-range"},
    "enum": {2: "enum-value", 3: "enum-options"},
    "service": {2: "method"},
    "field": {8: "field-options"},
}


def shrink_descriptor(data: bytes, preserved, budget: int) -> bytes:
    root = mutate.parse(data)

    def shrink_node(node: mutate.Node, kind: str) -> None:
        original = list(node.fields)
        count = len(original)
        if count > 1:
            def test(chosen: list[int]) -> bool:
                node.fields = [original[index] for index in chosen]
                if preserved(root.encode()):
                    return True
                node.fields = original
                return False

            kept = ddmin_indices(count, test)
            node.fields = [original[index] for index in kept]

        for field in node.fields:
            child_kind = CHILD_KINDS.get(kind, {}).get(field.number)
            if child_kind is None or field.wire != 2:
                continue
            if field.child is None:
                try:
                    field.child = mutate.parse(field.value)
                except mutate.WireError:
                    continue
            shrink_node(field.child, child_kind)

    shrink_node(root, "fds")

    # Leaf value reduction: the shortest string and a zero varint, guarded by
    # the same verdict.  This is the "truncate a string / zero a varint" half
    # of harness-design.md section 5.
    def leaves(node: mutate.Node, limit: list[int]) -> None:
        for position, field in enumerate(node.fields):
            if field.child is not None:
                leaves(field.child, limit)
                continue
            if limit[0] <= 0:
                return
            if field.wire == 2:
                raw = field.value
                candidates = [raw[: max(len(raw) // 2, 0)], b"", b"a"]
            elif field.wire == 0:
                candidates = [0]
            else:
                candidates = []
            for candidate in candidates:
                if candidate == field.value:
                    continue
                if field.wire == 2:
                    replacement = mutate.Field(field.number, 2, candidate)
                else:
                    replacement = mutate.Field(field.number, 0, candidate)
                original = node.fields[position]
                node.fields[position] = replacement
                limit[0] -= 1
                if preserved(root.encode()):
                    break
                node.fields[position] = original
            if limit[0] <= 0:
                return

    leaves(root, [min(40, max(budget // 10, 4))])
    return root.encode()


# ---------------------------------------------------------------------------
# Corpus rewiring
# ---------------------------------------------------------------------------


def load_row(path: pathlib.Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def shrink_row(row: dict, args, work_root: pathlib.Path) -> dict:
    row_id = row["id"]
    input_path = REPO / row["input"]
    raw = input_path.read_bytes()
    work = work_root / row_id
    work.mkdir(parents=True, exist_ok=True)
    verdict = Verdict(row, work, args.codegen.resolve(), args.protoc.resolve(),
                      args.timeout)
    target = verdict.measure(raw)
    started = time.monotonic()
    limit = args.budget

    def preserved(payload: bytes) -> bool:
        if payload not in verdict.cache and verdict.evaluations >= limit:
            return False
        return verdict.preserved(payload, target)

    if row["kind"] == "source":
        minimized = shrink_proto(raw.decode("utf-8", "replace"),
                                 preserved, limit).encode("utf-8")
    else:
        minimized = shrink_descriptor(raw, preserved, limit)

    final = verdict.measure(minimized)
    verified = final == target
    return {
        "id": row_id,
        "kind": row["kind"],
        "input": row["input"],
        "raw": raw,
        "size_before": len(raw),
        "size_after": len(minimized) if verified else len(raw),
        "evaluations": verdict.evaluations,
        "duration_s": round(time.monotonic() - started, 2),
        "verified": verified,
        "target_verdict": target,
        "final_verdict": final,
        "minimized": minimized if verified else raw,
        "raw_kept": not verified,
    }


def write_row(row: dict, outcome: dict) -> None:
    directory = CORPUS_GENERATED / row["id"]
    directory.mkdir(parents=True, exist_ok=True)
    input_name = pathlib.Path(row["input"]).name
    minimized_path = directory / input_name
    if outcome["verified"]:
        minimized_path.write_bytes(outcome["minimized"])
    else:
        raw_dir = directory / "raw"
        raw_dir.mkdir(exist_ok=True)
        (raw_dir / input_name).write_bytes(outcome["raw"])
    sidecar_path = CORPUS_ROWS / (row["id"] + ".expect.json")
    sidecar = load_row(sidecar_path)
    sidecar["minimized"] = {
        "verified": outcome["verified"],
        "size_before": outcome["size_before"],
        "size_after": outcome["size_after"],
        "evaluations": outcome["evaluations"],
        "duration_s": outcome["duration_s"],
        "raw_kept": outcome["raw_kept"],
        "shrunk_at": dt.datetime.now().astimezone().isoformat(
            timespec="seconds"),
        "tool": "shrink.py",
    }
    sidecar_path.write_text(seed.serialise(sidecar), encoding="utf-8",
                            newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true",
                        help="shrink every generated corpus row")
    parser.add_argument("--row", action="append", default=None,
                        help="shrink one row id; repeatable")
    parser.add_argument("--budget", type=int, default=400,
                        help="evaluation budget per row")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--protoc", type=pathlib.Path,
                        default=run.DEFAULT_PROTOC)
    parser.add_argument("--codegen", type=pathlib.Path,
                        default=run.DEFAULT_CODEGEN)
    parser.add_argument("--out", type=pathlib.Path, default=None)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if not args.all and not args.row:
        parser.error("give --all or at least one --row")
    if not args.codegen.is_file() or not args.protoc.is_file():
        print("tool missing", file=sys.stderr)
        return 2

    rows = [load_row(path) for path in sorted(CORPUS_ROWS.glob("*.expect.json"))]
    generated = [row for row in rows
                 if row.get("source") == "generated" and row.get("input")]
    if args.row:
        wanted = set(args.row)
        generated = [row for row in generated if row["id"] in wanted]
        missing = wanted - {row["id"] for row in generated}
        if missing:
            print("unknown row(s): %s" % ", ".join(sorted(missing)),
                  file=sys.stderr)
            return 2

    run_id = dt.datetime.now().strftime("shrink-%Y%m%d-%H%M%S")
    work_root = (args.out or (HERE / "runs" / run_id)).resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    print("work: %s" % work_root)
    print("rows: %d" % len(generated))

    outcomes = []
    overall = time.monotonic()
    for index, row in enumerate(generated, start=1):
        outcome = shrink_row(row, args, work_root)
        outcomes.append(outcome)
        if outcome["verified"]:
            write_row(row, outcome)
        if not args.quiet:
            print("[%3d/%3d] %-28s %5d -> %5d bytes  %4d evals  %6.2fs  %s" % (
                index, len(generated), row["id"], outcome["size_before"],
                outcome["size_after"], outcome["evaluations"],
                outcome["duration_s"],
                "ok" if outcome["verified"] else "VERDICT CHANGED"))

    failed = [item for item in outcomes if not item["verified"]]
    unchanged = [item for item in outcomes
                 if item["verified"]
                 and item["size_after"] == item["size_before"]]
    summary = {
        "schema": 1,
        "step": "B2b",
        "component": "shrinker",
        "run_id": run_id,
        "started_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "command": [sys.executable] + sys.argv,
        "rows": len(outcomes),
        "verified": len(outcomes) - len(failed),
        "verdict_changed": len(failed),
        "unchanged": len(unchanged),
        "bytes_before": sum(item["size_before"] for item in outcomes),
        "bytes_after": sum(item["size_after"] for item in outcomes),
        "evaluations": sum(item["evaluations"] for item in outcomes),
        "duration_s": round(time.monotonic() - overall, 2),
        "failures": [{"id": item["id"],
                      "target": item["target_verdict"],
                      "final": item["final_verdict"]} for item in failed],
        "rows_detail": [
            {"id": item["id"], "kind": item["kind"],
             "size_before": item["size_before"],
             "size_after": item["size_after"],
             "evaluations": item["evaluations"],
             "duration_s": item["duration_s"],
             "verified": item["verified"],
             "raw_kept": item["raw_kept"]}
            for item in outcomes
        ],
        "tools": {
            "codegen": str(args.codegen.resolve()),
            "codegen_sha256": run.runner.sha256_file(args.codegen.resolve()),
            "protoc": str(args.protoc.resolve()),
        },
        "note": ("a minimized input is stored as the corpus row; the raw input "
                 "is kept under raw/ only when the verdict changed"),
    }
    run.write_json(work_root / "shrink-summary.json", summary)
    run.write_json(HERE / "shrink-summary.json", summary)

    print()
    print("verified:     %d / %d" % (summary["verified"], summary["rows"]))
    print("verdict changed: %d" % summary["verdict_changed"])
    print("bytes:        %d -> %d" % (summary["bytes_before"],
                                       summary["bytes_after"]))
    print("evaluations:  %d" % summary["evaluations"])
    print("duration:     %.1fs" % summary["duration_s"])
    print("summary:      %s" % (work_root / "shrink-summary.json"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
