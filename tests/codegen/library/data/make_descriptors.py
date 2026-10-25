#!/usr/bin/env python3
"""make_descriptors.py - rebuild the library lane's `.pbs` fixtures.

The lane drives the C++ API and the CLI over the same descriptor sets. Every
binary fixture is produced here and nowhere else, so a fixture cannot drift
away from the shape it claims to have. The lane itself never calls protoc: the
`.pbs` files are committed.

Two kinds of fixtures:

- the shapes protoc accepts (`options-default`, `w47-external`,
  `w47-external-enum-default`, `w48-wire`) are compiled with
  `--descriptor_set_out`. `w47-external` and `w47-external-enum-default` are
  emitted WITHOUT `--include_imports`, so each is the thin descriptor W-47 is
  about: the root file names its dependency but the set does not carry it;
- the shapes protoc refuses to build from source - a message field with a
  default, and an enum default that is not an identifier - are encoded from
  the `.txtpb` beside them with
  `--encode=google.protobuf.FileDescriptorSet`. The text format does not
  validate them; it only encodes the descriptor the fixture names.

Usage:

    python tests/codegen/library/data/make_descriptors.py [--protoc PATH]

The script needs protoc (30.1 is the oracle this project measures against)
and google/protobuf/descriptor.proto. The latter is looked up in this order:

    1. $EASYPB_DESCRIPTOR_PROTO (a directory holding google/protobuf/descriptor.proto)
    2. build/protobuf-30.1-src/protobuf-30.1/src (the repo-local scratch copy)
    3. tests/codegen/parser/differential/corpus (the flat copy the repository
       already tracks for the differential lane)
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

# (descriptor-set stem, source stem, extra protoc flags). The two stems differ
# only for the enum control, whose output is named for its role and whose source
# names the shape it declares. The P7 pair is the CR-08 thin/full triple: the
# `.proto` root compiled WITHOUT `--include_imports` (the thin compatibility
# set) and WITH it (the complete closure the header must equal).
P7_EXT = os.path.join(HERE, "p7-ext")
PROTOC_SOURCES = (
    ("options-default", "options-default", ()),
    ("w47-external", "w47-external", ()),
    ("w47-external-enum-default", "w47-external-enum", ()),
    ("w48-wire", "w48-wire", ()),
    ("p7-thin", "b", ("--proto_path=" + P7_EXT,)),
    ("p7-full", "b",
     ("--proto_path=" + P7_EXT, "--include_imports")),
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

    for stem, source, flags in PROTOC_SOURCES:
        target = os.path.join(HERE, stem + ".pbs")
        command = [protoc, "--proto_path=" + HERE,
                   "--descriptor_set_out=" + target]
        command.extend(flags)
        command.append(source + ".proto")
        run(command)
        print("wrote %s (%d bytes)" % (os.path.basename(target),
                                       os.path.getsize(target)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
