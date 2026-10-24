#!/usr/bin/env python3
"""make_descriptors.py - rebuild the names lane's `.pbs` fixtures.

The lane drives two input paths, so every fixture exists twice: as `.proto`
source text and as a `.pbs` descriptor set. This script writes the descriptor
halves and is the only way they are produced, so a fixture can never drift
away from the shape it claims to have.

Most fixtures come from protoc: the `.pbs` is protoc's own encoding of the
`.proto` beside it, produced with `--descriptor_set_out`.

FIVE fixtures are crafted, because no `.proto` text can produce them. Four
declare a descriptor-origin name that is not a C++ identifier (`0Zc`,
`0Xb`, `0M`, `0E`): the source lexer cannot spell a digit-first name, so protoc
cannot compile a `.proto` into that descriptor, although protoc accepts such a
descriptor set. The fifth, `escape-enum-value`, declares the two enumerators
`class` and `class_`, and the reference compiler refuses that schema outright
("Enum name class_ has the same name as class if you ignore case and strip out
the enum name prefix"), so its descriptor cannot come from `--descriptor_set_out`
either. The descriptor of each crafted fixture is encoded from the `.txtpb`
beside it with `--encode=google.protobuf.FileDescriptorSet`.

Usage:

    python tests/codegen/names/data/make_descriptors.py [--protoc PATH]

The script needs protoc (30.1 is the oracle this project measures against)
and google/protobuf/descriptor.proto. The latter is looked up in this order:

    1. $EASYPB_DESCRIPTOR_PROTO (a directory holding google/protobuf/descriptor.proto)
    2. build/protobuf-30.1-src/protobuf-30.1/src (the repo-local scratch copy)
    3. tests/codegen/parser/differential/corpus (the flat copy the repository
       already tracks for the differential lane)

Nothing here is needed to RUN the lane: the `.pbs` files are committed, and
the lane itself never calls protoc.
"""

import argparse
import io
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(HERE))))
DEFAULT_PROTOC = r"C:\Base\Tools\BIN\protoc.exe"
DESCRIPTOR_DIRS = [
    os.path.join(REPO, "build", "protobuf-30.1-src", "protobuf-30.1", "src"),
    os.path.join(REPO, "tests", "codegen", "parser", "differential", "corpus"),
]

# The fixtures protoc refuses: their descriptor half is crafted, not compiled.
CRAFTED_FIXTURES = ("escape-enum-value",)


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


def find_descriptor_dir():
    from_env = os.environ.get("EASYPB_DESCRIPTOR_PROTO")
    if from_env:
        if os.path.exists(os.path.join(from_env, "google", "protobuf",
                                       "descriptor.proto")):
            return from_env
        fail("EASYPB_DESCRIPTOR_PROTO=%s holds no google/protobuf/descriptor.proto"
             % from_env)
    for candidate in DESCRIPTOR_DIRS:
        if os.path.exists(os.path.join(candidate, "google", "protobuf",
                                       "descriptor.proto")):
            return candidate
        if os.path.exists(os.path.join(candidate, "descriptor.proto")):
            return candidate
    fail("no google/protobuf/descriptor.proto found; set "
         "EASYPB_DESCRIPTOR_PROTO to the directory that holds it")


def run(command, stdin_bytes=None):
    result = subprocess.run(command, input=stdin_bytes, capture_output=True)
    if result.returncode != 0:
        fail("command failed (%d): %s\n%s"
             % (result.returncode, " ".join(command),
                result.stderr.decode("utf-8", "replace")))
    return result.stdout


def write(path, data):
    with io.open(path, "wb") as handle:
        handle.write(data)
    print("wrote %s (%d bytes)" % (os.path.basename(path), len(data)))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--protoc", default="", help="the protoc executable")
    args = parser.parse_args()

    protoc = find_protoc(args.protoc)
    descriptor_dir = find_descriptor_dir()

    # The crafted fixtures: a descriptor set encoded from a text file.
    for name in sorted(os.listdir(HERE)):
        if not name.endswith(".txtpb"):
            continue
        stem = name[:-len(".txtpb")]
        with io.open(os.path.join(HERE, name), "rb") as handle:
            text = handle.read()
        data = run([protoc, "--encode=google.protobuf.FileDescriptorSet",
                    "--proto_path=" + descriptor_dir,
                    "google/protobuf/descriptor.proto"], text)
        write(os.path.join(HERE, stem + ".pbs"), data)

    # The compiled fixtures: protoc's own encoding of the source beside them.
    for name in sorted(os.listdir(HERE)):
        if not name.endswith(".proto"):
            continue
        stem = name[:-len(".proto")]
        if stem in CRAFTED_FIXTURES:
            continue
        target = os.path.join(HERE, stem + ".pbs")
        run([protoc, "--proto_path=" + HERE, "--descriptor_set_out=" + target,
             os.path.basename(name)])
        print("wrote %s (%d bytes)" % (os.path.basename(target),
                                       os.path.getsize(target)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
