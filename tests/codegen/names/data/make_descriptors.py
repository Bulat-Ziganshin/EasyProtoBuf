#!/usr/bin/env python3
"""make_descriptors.py - rebuild the names lane's `.pbs` fixtures.

The lane drives two input paths, so every fixture exists twice: as `.proto`
source text and as a `.pbs` descriptor set. This script writes the descriptor
halves and is the only way they are produced, so a fixture can never drift
away from the shape it claims to have.

Every fixture comes from protoc: the `.pbs` is protoc's own encoding of the
`.proto` beside it, produced with `--descriptor_set_out`. Nothing here is
crafted, because every shape the lane covers is a shape protoc accepts - a
keyword in a declaration name, and two fields whose names collide.

Usage:

    python tests/codegen/names/data/make_descriptors.py [--protoc PATH]

The script needs protoc (30.1 is the oracle this project measures against).

Nothing here is needed to RUN the lane: the `.pbs` files are committed, and
the lane itself never calls protoc.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PROTOC = r"C:\Base\Tools\BIN\protoc.exe"


def fail(message):
    sys.stderr.write(message + "\n")
    raise SystemExit(1)


def find_protoc(explicit):
    if explicit:
        return explicit
    from_env = os.environ.get("EASYPB_PROTOC")
    if from_env:
        return from_env
    if os.path.exists(DEFAULT_PROTOC):
        return DEFAULT_PROTOC
    return "protoc"


def run(command):
    result = subprocess.run(command, capture_output=True)
    if result.returncode != 0:
        fail("command failed (%d): %s\n%s"
             % (result.returncode, " ".join(command),
                result.stderr.decode("utf-8", "replace")))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--protoc", default="", help="the protoc executable")
    args = parser.parse_args()

    protoc = find_protoc(args.protoc)

    for name in sorted(os.listdir(HERE)):
        if not name.endswith(".proto"):
            continue
        target = os.path.join(HERE, name[:-len(".proto")] + ".pbs")
        run([protoc, "--proto_path=" + HERE, "--descriptor_set_out=" + target,
             os.path.basename(name)])
        print("wrote %s (%d bytes)" % (os.path.basename(target),
                                       os.path.getsize(target)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
