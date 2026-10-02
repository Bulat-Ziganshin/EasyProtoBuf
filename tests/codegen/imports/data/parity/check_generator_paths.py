#!/usr/bin/env python3
"""The path contract of the generator gate, pinned by a regression.

`check_generator.py` spawns `generate.py` with the corpus directory as the
child's cwd (that is where the corpus lives and is written). A relative
`--descriptor-proto-path` typed on the command line therefore belongs to the
INVOKING directory, and the gate must resolve it there before passing it on -
otherwise the gate looks for the caller's path under the corpus directory and
refuses a path that exists, naming a path the caller never wrote.

This script pins both halves and reads/writes nothing in the corpus:

  * resolution - a relative path resolves against the caller's cwd, in
    `generate.find_descriptor_proto` and through the gate's own
    `--resolve-only` mode, which reports exactly what a full run would use and
    nothing else;
  * the refusal - when the path does not exist, the output names the spelling
    the caller typed next to the path it resolved to, never only a corpus-joined
    path the caller never wrote.

Registered as the CTest row `codegen.imports.parity.generator.paths`; a missing
`--protoc` is fine, the resolution cases do not reach protoc.

Usage:
    python tests/codegen/imports/data/parity/check_generator_paths.py \
        [--protoc <path>]
"""

import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True  # importing generate must leave no __pycache__

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = HERE
for _ in range(5):
    REPO = os.path.dirname(REPO)
CHECKER = os.path.join(HERE, "check_generator.py")

sys.path.insert(0, HERE)
import generate  # noqa: E402  (the resolution itself is under test)

FAILURES = []
PROBES = 0


def check(condition, message):
    global PROBES
    PROBES += 1
    if not condition:
        FAILURES.append(message)
        print("FAIL: " + message)
    return condition


def owned_snapshot():
    """sha256 of every file the generator owns - resolution must touch none."""
    result = {}
    for dirname in ("source", "set"):
        root = os.path.join(HERE, dirname)
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames.sort()
            for name in sorted(filenames):
                path = os.path.join(dirpath, name)
                with io.open(path, "rb") as handle:
                    result[path] = hashlib.sha256(handle.read()).hexdigest()
    with io.open(os.path.join(HERE, "parity-cases.tsv"), "rb") as handle:
        result["parity-cases.tsv"] = hashlib.sha256(handle.read()).hexdigest()
    return result


def run_checker(args, cwd):
    done = subprocess.run([sys.executable, CHECKER] + args, cwd=cwd,
                          capture_output=True)
    text = ((done.stdout or b"") + (done.stderr or b"")).decode("utf-8", "replace")
    return done.returncode, text


def case_library_resolution(given_missing):
    """`generate.find_descriptor_proto` resolves a relative path from the cwd."""
    mark = len(FAILURES)
    tmp = tempfile.mkdtemp(prefix="easypb-parity-paths-")
    try:
        with io.open(os.path.join(tmp, "descriptor.proto"), "w",
                     encoding="utf-8", newline="\n") as handle:
            handle.write("// probe: existence is all the lookup reads\n")
        relative = os.path.relpath(tmp, os.getcwd())
        src, name = generate.find_descriptor_proto(None, relative)
        check(src == os.path.abspath(relative) and name == "descriptor.proto",
              "find_descriptor_proto('%s') = ('%s', '%s'), expected ('%s', "
              "'descriptor.proto')" % (relative, src, name,
                                       os.path.abspath(relative)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    src, name = generate.find_descriptor_proto(None, given_missing)
    check(src == os.path.abspath(given_missing) and
          name == "google/protobuf/descriptor.proto",
          "a missing relative path must still resolve against the cwd: "
          "got ('%s', '%s')" % (src, name))
    if len(FAILURES) == mark:
        print("PATHS: resolution in find_descriptor_proto ok")


def case_resolve_only():
    """The gate's --resolve-only reports the caller-cwd resolution, and no more."""
    mark = len(FAILURES)
    tracked = "tests/codegen/parser/differential/corpus"
    tracked_abs = os.path.abspath(os.path.join(REPO, tracked))
    code, text = run_checker(["--resolve-only",
                              "--descriptor-proto-path", tracked], REPO)
    check(code == 0, "--resolve-only with the tracked corpus exited %d:\n%s"
                     % (code, text))
    check(tracked_abs in text,
          "--resolve-only did not print the caller-cwd resolution '%s':\n%s"
          % (tracked_abs, text))
    check("--descriptor-proto-path was given as '%s' and resolved to %s"
          % (tracked, tracked_abs) in text,
          "--resolve-only did not name the typed spelling next to the "
          "resolution:\n%s" % text)
    check("descriptor.proto" in text,
          "--resolve-only did not name the descriptor file:\n%s" % text)

    code, text = run_checker(["--resolve-only"], REPO)
    check(code == 0, "--resolve-only without the argument exited %d:\n%s"
                     % (code, text))
    check("PARITY-GEN: descriptor " in text,
          "--resolve-only did not print the default resolution:\n%s" % text)
    if len(FAILURES) == mark:
        print("PATHS: --resolve-only ok")


def case_refusal_names_the_given_path(given):
    """A missing relative path is refused naming the caller's own spelling."""
    mark = len(FAILURES)
    corpus_joined = os.path.abspath(os.path.join(HERE, given))
    caller_joined = os.path.abspath(os.path.join(REPO, given))
    check(corpus_joined != caller_joined,
          "the regression is vacuous: the cwd and the corpus directory coincide")
    code, text = run_checker(["--descriptor-proto-path", given], REPO)
    check(code != 0, "a missing descriptor path exited 0:\n%s" % text)
    check("--descriptor-proto-path was given as '%s'" % given in text,
          "the refusal does not carry the 'given as' clause:\n%s" % text)
    check(caller_joined in text,
          "the refusal does not name the caller-cwd resolution '%s':\n%s"
          % (caller_joined, text))
    check(corpus_joined not in text,
          "the refusal still names the corpus-joined path '%s':\n%s"
          % (corpus_joined, text))
    if len(FAILURES) == mark:
        print("PATHS: refusal ok")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--protoc", default=None,
                        help="passed through to the gate; the resolution cases "
                             "do not need it")
    args = parser.parse_args()

    before = owned_snapshot()
    case_library_resolution("no-such-descriptor-dir/probe-src")
    case_resolve_only()
    case_refusal_names_the_given_path("no-such-descriptor-dir/descriptor-src")
    if args.protoc:  # the same refusal through the row's own invocation shape
        code, text = run_checker(
            ["--protoc", args.protoc,
             "--descriptor-proto-path", "no-such-descriptor-dir/descriptor-src"],
            REPO)
        check(code != 0 and
              "--descriptor-proto-path was given as "
              "'no-such-descriptor-dir/descriptor-src'" in text,
              "the protoc-shaped invocation did not refuse by name:\n%s" % text)

    after = owned_snapshot()
    check(before == after, "the corpus changed: this script must only read it")

    if FAILURES:
        print("PATHS: %d probe(s), %d failure(s)" % (PROBES, len(FAILURES)))
        return 1
    print("PATHS: %d probe(s), 0 failure(s) - the path contract holds" % PROBES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
