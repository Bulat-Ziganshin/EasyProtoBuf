#!/usr/bin/env python3
"""The protoc half of Step 11's two-way oracle parity lane.

`parity-cases.tsv` records, per fixture, the verdict `protoc` answered with.
This script re-derives that cell from the fixture and fails when it has moved,
so the recorded verdicts are measurements rather than folklore:

  * source-origin rows: one ordinary `--proto_path` compile of
    `source/<case>.proto`;
  * descriptor-origin rows: the tracked `set/<case>.pbs` handed back to protoc
    with `--descriptor_set_in` and NO `--proto_path`, run from an empty
    directory - protocol A13, because with a proto path that holds the same
    `.proto` protoc recompiles the source and never reads the set (the `D-24`
    counter-example). When `--descriptor-proto-path` points at a directory that
    holds `google/protobuf/descriptor.proto`, the check is deepened: the
    authored `set/<case>.txtpb` is re-encoded and the bytes are compared with
    the tracked `.pbs`, which is what keeps the readable text and the binary in
    step. Without that path the re-encode is skipped BY NAME, never silently.

It mirrors `tests/codegen/parser/differential/run.py`: it is the opt-in half of
the lane (`EASYPB_BUILD_DIFFERENTIAL_TESTS`' condition, plus a `--protoc`
argument), it prints `PARITY: compared <n> row(s)`, it names every row it could
not run with the pack's `SKIP:` marker, and it records `protoc --version` - the
version is the lane's arbiter and the lane must not depend on remembering it.

Fixtures that carry no row (`set/probe-*`) hold the same text/binary invariant
as a row's, and are swept too: under `--descriptor-proto-path` every tracked
`.txtpb` that the table does not read is re-encoded and compared with its
`.pbs`, and a `.pbs` without its `.txtpb` fails by name.

The linker half of the same table is the CTest rows `codegen.imports.parity`
(the descriptor-origin rows, every profile) and `codegen.imports.parity.source`
(the source-origin rows, where the loader is available); see
`tests/codegen/imports/test_parity.cpp`.

Usage:
    python tests/codegen/imports/data/parity/run.py \
        --protoc C:/Base/Tools/BIN/protoc.exe \
        --descriptor-proto-path build/protobuf-30.1-src/protobuf-30.1/src
"""

import argparse
import io
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "source")
SETS = os.path.join(HERE, "set")
TSV = os.path.join(HERE, "parity-cases.tsv")
DESCRIPTOR_PROTO = "google/protobuf/descriptor.proto"


def read_rows(path):
    """[(case, origin, protoc, expected, kind, note)] from the tsv."""
    rows = []
    with io.open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n").rstrip("\r")
            if not line or line.startswith("case\t"):
                continue
            fields = line.split("\t")
            if len(fields) != 6:
                raise SystemExit("%s: expected 6 tab-separated fields, found %d"
                                 % (path, len(fields)))
            rows.append(fields)
    return rows


def run(args, cwd, stdin_bytes=None):
    done = subprocess.run(args, cwd=cwd, input=stdin_bytes,
                          capture_output=True)
    text = (done.stdout or b"").decode("utf-8", "replace") + \
           (done.stderr or b"").decode("utf-8", "replace")
    return done.returncode, text.strip()


def source_verdict(protoc, case, scratch):
    """One ordinary compile of the source fixture."""
    root = os.path.join(SOURCE, case + ".proto")
    return run([protoc, "--proto_path=" + SOURCE,
                "--descriptor_set_out=" + os.path.join(scratch, case + ".pb"),
                root], scratch)


def descriptor_verdict(protoc, case, scratch):
    """A13: build the file FROM the tracked set, with no --proto_path."""
    pbs = os.path.join(SETS, case + ".pbs")
    build_dir = os.path.join(scratch, case + "-build")
    os.makedirs(build_dir)
    out = os.path.join(build_dir, "cpp")
    os.makedirs(out)
    return run([protoc, "--descriptor_set_in=" + pbs,
                "--cpp_out=" + out, "root.proto"], build_dir)


def reencode_bytes(protoc, case, descriptor_proto_path, scratch):
    """protoc's encoding of the authored textproto, or (None, reason)."""
    txtpb = os.path.join(SETS, case + ".txtpb")
    encode_dir = os.path.join(scratch, case + "-encode")
    os.makedirs(encode_dir)
    with io.open(txtpb, "rb") as handle:
        authored = handle.read()
    done = subprocess.run(
        [protoc, "--proto_path=" + descriptor_proto_path,
         "--encode=google.protobuf.FileDescriptorSet", DESCRIPTOR_PROTO],
        cwd=encode_dir, input=authored, capture_output=True)
    if done.returncode != 0:
        reason = (done.stderr or b"").decode("utf-8", "replace").strip()
        return None, reason.splitlines()[0] if reason else "no output"
    return done.stdout, ""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--protoc", required=True,
                        help="path to the official protoc executable")
    parser.add_argument("--descriptor-proto-path", default=None,
                        help="a proto path holding google/protobuf/"
                             "descriptor.proto; enables the re-encode check "
                             "for descriptor-origin rows")
    parser.add_argument("--case", action="append", dest="cases", default=None,
                        help="run only the named fixture(s)")
    args = parser.parse_args()

    if not os.path.exists(args.protoc):
        print("SKIP: every row - no protoc at " + args.protoc)
        print("PARITY: compared 0 row(s)")
        return 1
    # protoc runs from a scratch cwd (see descriptor_verdict), so a caller's
    # relative proto path has to be resolved here or it silently points nowhere.
    descriptor_proto_path = (os.path.abspath(args.descriptor_proto_path)
                             if args.descriptor_proto_path else None)

    version = run([args.protoc, "--version"], HERE)[1]
    print("PARITY: protoc " + (version or "version unknown"))

    selected = set(args.cases or [])
    rows = read_rows(TSV)
    scratch = tempfile.mkdtemp(prefix="easypb-parity-protoc-")
    compared = 0
    skipped = []
    failures = []
    try:
        row_names = set(row[0] for row in rows)
        for case, origin, recorded, expected, kind, note in rows:
            if selected and case not in selected:
                continue
            if origin == "source":
                code, text = source_verdict(args.protoc, case, scratch)
            elif origin == "descriptor":
                code, text = descriptor_verdict(args.protoc, case, scratch)
                if descriptor_proto_path:
                    encoded, reason = reencode_bytes(
                        args.protoc, case, descriptor_proto_path, scratch)
                    if encoded is None:
                        failures.append("%s: re-encode failed: %s"
                                        % (case, reason))
                    else:
                        with io.open(os.path.join(SETS, case + ".pbs"),
                                     "rb") as handle:
                            tracked = handle.read()
                        if tracked != encoded:
                            failures.append(
                                "%s: the tracked .pbs is not protoc's encoding "
                                "of its .txtpb" % case)
                else:
                    skipped.append(case + ": re-encode check needs "
                                        "--descriptor-proto-path")
            else:
                failures.append("%s: unknown origin %r" % (case, origin))
                continue

            verdict = "accept" if code == 0 else "reject"
            compared += 1
            first = ""
            for line in text.splitlines():
                if line.strip():
                    first = line.strip()
                    break
            if verdict != recorded:
                failures.append("%s: recorded protoc=%s, re-derived protoc=%s "
                                "(%s)" % (case, recorded, verdict, first))
            print("PARITY: %-52s recorded=%s re-derived=%s %s"
                  % (case, recorded, verdict, first[:80]))

        # The fixtures without a row: `set/probe-*` are measured evidence, and
        # the same invariant covers them - the tracked `.pbs` is protoc's own
        # encoding of the tracked `.txtpb`. They are named one by one, so a
        # sweep that read nothing is visible.
        probe_count = 0
        if descriptor_proto_path:
            for entry in sorted(os.listdir(SETS)):
                if not entry.endswith(".pbs"):
                    continue
                case = entry[:-len(".pbs")]
                if case in row_names:
                    continue
                if not os.path.exists(os.path.join(SETS, case + ".txtpb")):
                    failures.append(
                        "%s: a tracked .pbs without its .txtpb" % case)
                    continue
                encoded, reason = reencode_bytes(
                    args.protoc, case, descriptor_proto_path, scratch)
                if encoded is None:
                    failures.append("%s: probe re-encode failed: %s"
                                    % (case, reason))
                else:
                    with io.open(os.path.join(SETS, case + ".pbs"),
                                 "rb") as handle:
                        tracked = handle.read()
                    if tracked != encoded:
                        failures.append(
                            "%s: the tracked probe .pbs is not protoc's "
                            "encoding of its .txtpb" % case)
                probe_count += 1
                print("PARITY: %-52s probe (no row), bytes compared" % case)
        print("PARITY: %d fixture(s) without a row re-encoded beside the rows"
              % probe_count)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    for reason in skipped:
        print("SKIP: " + reason)
    for failure in failures:
        print("FAIL: " + failure)
    print("PARITY: compared %d row(s), %d skipped, %d failure(s)"
          % (compared, len(skipped), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
