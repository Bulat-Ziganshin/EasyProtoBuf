#!/usr/bin/env python3
"""make_descriptors.py - rebuild the validity lane's `.pbs` fixtures.

The lane drives two input paths, so every fixture exists twice: as `.proto`
source text and as a `.pbs` descriptor set. This script writes the descriptor
halves and is the only way they are produced, so a fixture can never drift
away from the shape it claims to have.

Two shapes come from protoc:

- the paired fixtures (`defaults-*.proto`) are compiled with
  `--descriptor_set_out`, so the descriptor is protoc's own encoding of the
  source file and carries the same `default_value` spelling protoc writes;
- the crafted fixtures (`*.txtpb`) are encoded with
  `--encode=google.protobuf.FileDescriptorSet`, because protoc cannot produce
  them from source text: an enum value named `1A`, a field whose type is 99, a
  field with no type at all, a proto2 group, and a file name with a newline.

ONE crafted shape needs one byte that protoc's text format will not carry:
`field-type-unknown` declares `type: 99`, and the text format answers
"Unknown enumeration value of \"99\" for field \"type\"". The script therefore
encodes that fixture TWICE - once with `type: TYPE_INT32` and once with
`type: TYPE_SINT32`, both one-byte varints - asserts that the two encodings
differ in exactly one byte, and writes 99 into that byte. The result is
protoc's own encoding of the same descriptor with the unknown kind.

Usage:

    python tests/codegen/validity/data/make_descriptors.py [--protoc PATH]

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

# The one fixture whose text protoc will not parse, and the two legal type
# values it is encoded with instead (both are one-byte varints, so the two
# encodings differ in exactly one byte - the kind itself).
PATCHED_TYPE_FIXTURE = "field-type-unknown"
PATCHED_TYPE_VALUE = 99
PATCHED_TYPE_PROBES = ("TYPE_INT32", "TYPE_SINT32")


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


def patched_kind_bytes(protoc, descriptor_dir, text):
    """The encoding of `text` with the field's unknown kind written in.

    The text format refuses an enumeration value it does not know, so the
    fixture is encoded with two legal one-byte kinds and the single byte they
    disagree on is replaced. The assertion is the point: if the two encodings
    ever differ in more than one byte, the patch would be a guess.
    """
    encodings = []
    decoded = text.decode("utf-8")
    for probe in PATCHED_TYPE_PROBES:
        replaced = decoded.replace("type: %d" % PATCHED_TYPE_VALUE,
                                   "type: %s" % probe)
        if replaced == decoded:
            fail("%s.txtpb does not declare 'type: %d'"
                 % (PATCHED_TYPE_FIXTURE, PATCHED_TYPE_VALUE))
        encodings.append(encode_textproto(protoc, descriptor_dir,
                                           replaced.encode("utf-8")))
    first, second = encodings
    if len(first) != len(second):
        fail("the two encodings of %s differ in length" % PATCHED_TYPE_FIXTURE)
    differences = [index for index in range(len(first))
                   if first[index] != second[index]]
    if len(differences) != 1:
        fail("the two encodings of %s differ in %d bytes, expected exactly one"
             % (PATCHED_TYPE_FIXTURE, len(differences)))
    patched = bytearray(first)
    patched[differences[0]] = PATCHED_TYPE_VALUE
    return bytes(patched)


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
        if stem == PATCHED_TYPE_FIXTURE:
            data = patched_kind_bytes(protoc, descriptor_dir, text)
        else:
            data = encode_textproto(protoc, descriptor_dir, text)
        write(os.path.join(HERE, stem + ".pbs"), data)

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
