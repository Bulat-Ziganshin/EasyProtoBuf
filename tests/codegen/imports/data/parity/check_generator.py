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

A relative `--descriptor-proto-path` is resolved against the directory this
script is INVOKED from, not against this directory: the generator is spawned
with the corpus directory as its cwd, so a path passed through verbatim would
be looked up under the corpus - a path the caller never typed
(`codegen.imports.parity.generator.paths` is the regression). `--resolve-only`
prints what a full run would use and touches nothing.

Usage:
    python tests/codegen/imports/data/parity/check_generator.py \
        [--protoc <path>] [--descriptor-proto-path <dir>] [--resolve-only]
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


def caller_absolute(value):
    """A relative command-line path belongs to the INVOKING directory.

    `run_generator` spawns the generator with `cwd=HERE`, so a relative
    `--descriptor-proto-path` passed through verbatim would be resolved against
    the corpus directory - a path the caller never typed, and not the one it
    meant. Resolving it here, against this process's own working directory, is
    the fix; the CTest row `codegen.imports.parity.generator.paths` is its
    regression.
    """
    if not value:
        return None
    return os.path.abspath(value)


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


def resolve_only(args, given):
    """Print the toolchain a full run would use; write nothing anywhere."""
    sys.dont_write_bytecode = True  # the import must leave no __pycache__
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import generate  # the single source of the resolution order
    protoc = args.protoc or generate.PROTOC
    src, name = generate.find_descriptor_proto(protoc, args.descriptor_proto_path)
    print("PARITY-GEN: descriptor " + os.path.join(src, name))
    if given:
        print("PARITY-GEN: --descriptor-proto-path was given as '%s' and "
              "resolved to %s" % (given, src))
    if not os.path.exists(os.path.join(src, name)):
        print("no %s under %s (--descriptor-proto-path was given as '%s'): "
              "pass --descriptor-proto-path" % (name, src, given))
        return 2
    print("PARITY-GEN: resolve-only, nothing regenerated")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--protoc", default=None,
                        help="path to the official protoc executable; without "
                             "it generate.py resolves the default")
    parser.add_argument("--descriptor-proto-path", default=None,
                        help="a proto path holding descriptor.proto; a "
                             "relative path belongs to the directory this "
                             "script is invoked from")
    parser.add_argument("--resolve-only", action="store_true",
                        help="resolve protoc and the descriptor source, print "
                             "them and exit; nothing is read from or written "
                             "to the corpus (exit 2 when the source is absent)")
    args = parser.parse_args()

    given = args.descriptor_proto_path  # exactly as typed, for the messages
    args.descriptor_proto_path = caller_absolute(given)

    protoc_line = "not named (generate.py resolves it)"
    if args.protoc:
        done = subprocess.run([args.protoc, "--version"], capture_output=True)
        version = ((done.stdout or b"") + (done.stderr or b"")).decode(
            "utf-8", "replace").strip()
        protoc_line = version or args.protoc
    print("PARITY-GEN: protoc " + protoc_line)
    print("PARITY-GEN: generator " + GENERATOR)

    if args.resolve_only:
        return resolve_only(args, given)

    before = snapshot()
    code, text, last = run_generator(args)
    if code != 0:
        print(text)
        if given:
            print("FAIL: --descriptor-proto-path was given as '%s' and "
                  "resolved to %s" % (given, args.descriptor_proto_path))
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
