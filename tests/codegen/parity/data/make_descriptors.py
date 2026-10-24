#!/usr/bin/env python3
"""make_descriptors.py - rebuild the parity lane's `.pbs` fixtures.

The lane drives two input paths, so every rule it checks exists twice: as
`.proto` source text and as a `.pbs` descriptor set of the same shape. This
script writes the descriptor halves and is the only way they are produced, so
a fixture cannot drift away from the shape it claims to have.

Two kinds of fixtures:

- the shapes protoc itself accepts (`methods-ok`, `map-ok`, `enum-ok`,
  `ext-ok`, `w68-dep`, `w68-root`) are compiled with
  `--descriptor_set_out`. `w68-root` is included even though its import is
  missing from the SET: protoc emits only the listed file, which is exactly
  the thin descriptor W-68 is about;
- the shapes protoc refuses to build from source - duplicate method names, a
  method endpoint that does not exist, a reversed map entry, a proto3 enum
  whose first value is not zero, a proto3 or field-overlapping extension
  range, and the W-36 containers - are encoded from `.txtpb` with
  `--encode=google.protobuf.FileDescriptorSet`. The text format does not
  validate them; it only encodes the descriptor the fixture names.

Usage:

    python tests/codegen/parity/data/make_descriptors.py [--protoc PATH]

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

# The source fixtures protoc accepts. The others are intentional refusals and
# have no compiled half: their `.pbs` twin is encoded from the `.txtpb` beside
# it (or, for W-36, the containers themselves are the fixture).
PROTOC_SOURCES = (
    "methods-ok",
    "map-ok",
    "enum-ok",
    "ext-ok",
    "ext-max",
    "message-set-empty",
    "w68-dep",
    "w68-root",
)


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


def encode_textproto(protoc, descriptor_dir, text):
    return run([protoc, "--encode=google.protobuf.FileDescriptorSet",
                "--proto_path=" + descriptor_dir,
                "google/protobuf/descriptor.proto"], text)


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

    for name in sorted(os.listdir(HERE)):
        if not name.endswith(".txtpb"):
            continue
        stem = name[:-len(".txtpb")]
        with io.open(os.path.join(HERE, name), "rb") as handle:
            text = handle.read()
        write(os.path.join(HERE, stem + ".pbs"),
              encode_textproto(protoc, descriptor_dir, text))

    for stem in PROTOC_SOURCES:
        target = os.path.join(HERE, stem + ".pbs")
        run([protoc, "--proto_path=" + HERE, "--descriptor_set_out=" + target,
             stem + ".proto"])
        print("wrote %s (%d bytes)" % (os.path.basename(target),
                                       os.path.getsize(target)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
