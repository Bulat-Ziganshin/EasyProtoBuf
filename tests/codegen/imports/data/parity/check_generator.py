#!/usr/bin/env python3
"""The corpus generator's idempotence gate.

`parity-cases.tsv` and every fixture under `source/` and `set/` are the output
of `generate.py`, and this script asserts that claim mechanically: it hashes
every file the generator owns, runs the generator in place, hashes again, and
fails with the list of files that appeared, disappeared or changed. So the
corpus cannot drift from its generator without the CTest row
`codegen.imports.parity.generator` going red (registered only under
`EASYPB_BUILD_DIFFERENTIAL_TESTS`, and only when `EASYPB_PROTOC` and a Python
interpreter are both present).

The comparison is content-based rather than `git status`, on purpose: a handoff
snapshot without `.git` is checked the same way, and a changed byte is named
even where git would have seen nothing. The generator's own scratch log
(`build/codegen-parity/protoc-verdicts.txt`) is not part of the corpus and is
not compared.

Usage:
    python tests/codegen/imports/data/parity/check_generator.py \
        [--protoc <path>] [--descriptor-proto-path <dir>]
"""

import argparse
import hashlib
import io
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GENERATOR = os.path.join(HERE, "generate.py")
OWNED_DIRS = ("source", "set")
OWNED_FILE = "parity-cases.tsv"


def owned_files():
    """Every path the generator writes, in a stable order."""
    paths = [os.path.join(HERE, OWNED_FILE)]
    for name in OWNED_DIRS:
        root = os.path.join(HERE, name)
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames.sort()
            for filename in sorted(filenames):
                paths.append(os.path.join(dirpath, filename))
    return sorted(set(paths))


def digest(path):
    with io.open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def snapshot():
    return dict((path, digest(path)) for path in owned_files())


def relative(path):
    return os.path.relpath(path, HERE).replace("\\", "/")


def run_generator(args):
    command = [sys.executable, GENERATOR]
    if args.protoc:
        command += ["--protoc", args.protoc]
    if args.descriptor_proto_path:
        command += ["--descriptor-proto-path", args.descriptor_proto_path]
    done = subprocess.run(command, cwd=HERE, capture_output=True)
    text = (done.stdout or b"").decode("utf-8", "replace") + \
           (done.stderr or b"").decode("utf-8", "replace")
    last = ""
    for line in text.splitlines():
        if line.strip():
            last = line.strip()
    return done.returncode, text, last


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--protoc", default=None,
                        help="path to the official protoc executable; without "
                             "it generate.py resolves the default")
    parser.add_argument("--descriptor-proto-path", default=None,
                        help="a proto path holding descriptor.proto")
    args = parser.parse_args()

    protoc_line = "not named (generate.py resolves it)"
    if args.protoc:
        done = subprocess.run([args.protoc, "--version"], capture_output=True)
        version = ((done.stdout or b"") + (done.stderr or b"")).decode(
            "utf-8", "replace").strip()
        protoc_line = version or args.protoc
    print("PARITY-GEN: protoc " + protoc_line)
    print("PARITY-GEN: generator " + GENERATOR)

    before = snapshot()
    code, text, last = run_generator(args)
    if code != 0:
        print(text)
        print("FAIL: generate.py exited %d" % code)
        return 1
    after = snapshot()

    failures = []
    for path in sorted(set(before) | set(after)):
        name = relative(path)
        if path not in before:
            failures.append("%s: absent before the run and written by it - the "
                            "tracked corpus was not this generator's output"
                            % name)
        elif path not in after:
            failures.append("%s: present before the run and gone after it"
                            % name)
        elif before[path] != after[path]:
            failures.append("%s: sha256 %s -> %s"
                            % (name, before[path][:12], after[path][:12]))

    print("PARITY-GEN: " + last)
    print("PARITY-GEN: %d file(s) hashed before and after, %d change(s)"
          % (len(before), len(failures)))
    for failure in failures:
        print("FAIL: " + failure)
    if failures:
        return 1
    print("PARITY-GEN: the tracked corpus is this generator's output")
    return 0


if __name__ == "__main__":
    sys.exit(main())
