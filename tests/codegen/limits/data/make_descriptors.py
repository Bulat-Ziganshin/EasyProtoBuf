#!/usr/bin/env python3
"""make_descriptors.py - rebuild the limits lane's fixtures.

The lane drives two input paths, so every schema exists twice: as `.proto`
source text and as a `.pbs` descriptor set. This script writes BOTH halves,
so a fixture can never drift away from the shape it claims to have: a 31-level
schema and a 32-level schema are not writable by hand, and the pair is the
whole point of the lane.

Three shapes are produced:

- `nesting-31.proto` / `nesting-31.pbs` - 31 levels of nested messages, the
  deepest schema the reference compiler accepts. The `.pbs` is protoc's own
  encoding of the `.proto` beside it.

- `nesting-32.proto` / `nesting-32.pbs` - the same shape one level deeper.
  protoc REFUSES to build that descriptor (its MaxMessageDeclarationNestingDepth()
  is 32 and it refuses the 32nd level), so the descriptor half is encoded
  from the text format, which knows no such limit.

- `deep-name.pbs` - a descriptor whose logical name is a chain of 101
  components. The name is what drives the depth of the output tree the
  publisher creates, so it is the input the path bound has to refuse.

Usage:

    python tests/codegen/limits/data/make_descriptors.py [--protoc PATH]

The script needs protoc (30.1 is the oracle this project measures against)
and google/protobuf/descriptor.proto. The latter is looked up in this order:

    1. $EASYPB_DESCRIPTOR_PROTO (a directory holding google/protobuf/descriptor.proto)
    2. build/protobuf-30.1-src/protobuf-30.1/src (the repo-local scratch copy)
    3. tests/codegen/parser/differential/corpus (the flat copy the repository
       already tracks for the differential lane)

Nothing here is needed to RUN the lane: the fixtures are committed, and the
lane itself never calls protoc.
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

# The two nesting depths the lane drives. 31 is the deepest schema the
# reference compiler accepts; 32 is the first one it refuses.
ACCEPTED_DEPTH = 31
REFUSED_DEPTH = 32

# The number of components a logical name may have (the limit the lane proves):
# the name of `deep-name.pbs` carries one more than that.
DEEP_NAME_COMPONENTS = 101


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


def source_text(depth, name):
    """The `.proto` source of a chain of `depth` nested messages."""
    lines = ['syntax = "proto2";', ""]
    for level in range(depth):
        indent = "  " * level
        lines.append("%smessage M%d {" % (indent, level))
        lines.append("%s  optional int32 x = 1;" % indent)
    for level in range(depth):
        indent = "  " * level
        lines.append("%s}" % indent)
    return "\n".join(lines) + "\n"


def textproto(depth, name):
    """The text format of the same chain, for the shape protoc refuses."""
    lines = ["file {", '  name: "%s"' % name, "  message_type {"]
    for level in range(depth):
        indent = "  " * (level + 2)
        lines.append('%sname: "M%d"' % (indent, level))
        lines.append("%sfield { name: \"x\" number: 1 label: LABEL_OPTIONAL "
                     "type: TYPE_INT32 }" % indent)
        if level + 1 < depth:
            lines.append("%snested_type {" % indent)
    for level in range(depth):
        indent = "  " * (level + 2)
        lines.append("%s}" % indent)
    lines.append("}")
    return "\n".join(lines) + "\n"


def deep_name_textproto():
    """A descriptor whose logical name is a chain of components."""
    name = "/".join("d" for _ in range(DEEP_NAME_COMPONENTS - 1)) + "/x.proto"
    return ('file {\n'
            '  name: "%s"\n'
            '  message_type {\n'
            '    name: "M"\n'
            '    field { name: "x" number: 1 label: LABEL_OPTIONAL '
            'type: TYPE_INT32 }\n'
            '  }\n'
            '}\n' % name)


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

    # The accepted shape: protoc builds the descriptor itself.
    accepted_source = source_text(ACCEPTED_DEPTH, "nesting-31.proto")
    write(os.path.join(HERE, "nesting-31.proto"),
          accepted_source.encode("utf-8"))
    run([protoc, "--proto_path=" + HERE,
         "--descriptor_set_out=" + os.path.join(HERE, "nesting-31.pbs"),
         "nesting-31.proto"])
    print("wrote nesting-31.pbs (%d bytes)"
          % os.path.getsize(os.path.join(HERE, "nesting-31.pbs")))

    # The refused shape: protoc cannot build it, so the text format does.
    refused_source = source_text(REFUSED_DEPTH, "nesting-32.proto")
    write(os.path.join(HERE, "nesting-32.proto"), refused_source.encode("utf-8"))
    write(os.path.join(HERE, "nesting-32.pbs"),
          encode_textproto(protoc, descriptor_dir,
                           textproto(REFUSED_DEPTH, "nesting-32.proto")
                           .encode("utf-8")))

    # The deep name: a logical name with more components than the limit.
    write(os.path.join(HERE, "deep-name.pbs"),
          encode_textproto(protoc, descriptor_dir,
                           deep_name_textproto().encode("utf-8")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
