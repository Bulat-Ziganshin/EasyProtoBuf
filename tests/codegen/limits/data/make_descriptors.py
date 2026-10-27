#!/usr/bin/env python3
"""make_descriptors.py - rebuild the limits lane's fixtures.

The lane drives two input paths, so every schema exists twice: as `.proto`
source text and as a `.pbs` descriptor set. This script writes BOTH halves,
so a fixture can never drift away from the shape it claims to have: a 31-level
schema and a 32-level schema are not writable by hand, and the pair is the
whole point of the lane.

The mechanics - the protoc lookup, the `descriptor.proto` lookup, the
text-format encoder, the writer and the `--check` gate - live in
`tests/codegen/common/descriptor_fixtures.py`; this script is the lane's table
of generated fixtures.

Four shapes are produced:

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

- `dependency-chain.proto` / `dependency-chain.pbs` - 12 000 top-level
  messages, each holding the next one by value. The nesting depth is 1; the
  dependency graph is 12 000 edges deep, which is the axis the recursive
  cycle detector and topological sort used to overflow on (CR-35). The
  `.pbs` is protoc's own encoding of the `.proto` beside it.

- `public-chain-1000.pbs` / `public-chain-20000.pbs` - a chain of files, each
  one `import public`-ing the next. These are the descriptor half of the
  import-chain bound (review 1027, U-05/U-06): the graph arrives whole, with
  no filesystem to walk, so the linker owns the bound there. 1 000 files is
  the deepest chain the project accepts; 20 000 is the input that used to end
  the process in `public_exports`. There is no `.proto` half: a 20 000-file
  chain is not writable as one source file, and the source door is driven from
  files the lane writes when it runs. The sets are crafted, not compiled, for
  the same reason - but each is a well-formed `FileDescriptorSet` that protoc
  itself accepts as input.

Usage:

    python tests/codegen/limits/data/make_descriptors.py [--protoc PATH] [--check]

`--check` regenerates every `.proto` and `.pbs` in memory and compares it with
the committed file instead of writing it.

Nothing here is needed to RUN the lane: the fixtures are committed, and the
lane itself never calls protoc.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# `tests/codegen`, so `common.descriptor_fixtures` is importable from anywhere.
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
# Do not leave an untracked `common/__pycache__` in the repository.
sys.dont_write_bytecode = True

from common.descriptor_fixtures import (Fixture, build_fixtures,
                                        parse_args)

# The two nesting depths the lane drives. 31 is the deepest schema the
# reference compiler accepts; 32 is the first one it refuses.
ACCEPTED_DEPTH = 31
REFUSED_DEPTH = 32

# The number of components a logical name may have (the limit the lane proves):
# the name of `deep-name.pbs` carries one more than that.
DEEP_NAME_COMPONENTS = 101

# The dependency-chain length. 12 000 is the counter-example from the review:
# the recursive cycle detector and topological sort ended the process with
# STATUS_STACK_OVERFLOW (0xC00000FD) on this depth, while 6 000 messages
# passed. The chain is top-level throughout, so it is the graph depth, not
# the nesting depth, that the lane exercises.
CHAIN_LENGTH = 12000

# The import-chain bound of review 1027 (U-05/U-06): 1 000 files are accepted,
# 1 001 are refused. The public-import chain is the shape whose linker walk
# used to overflow the stack at ~1 950 links and whose per-file export sets
# used ~3 GB at 8 000; 20 000 is the length the review measured the crash at.
PUBLIC_CHAIN_ACCEPTED = 1000
PUBLIC_CHAIN_REFUSED = 20000


def source_text(depth):
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


def chain_source_text(length):
    """The `.proto` source of a by-value message chain."""
    lines = ['syntax = "proto3";', ""]
    for index in range(length):
        if index + 1 < length:
            lines.append("message M%d { M%d f = 1; }" % (index, index + 1))
        else:
            lines.append("message M%d { int32 f = 1; }" % index)
    return "\n".join(lines) + "\n"


def public_chain_textproto(length):
    """A descriptor set whose files form one `import public` chain.

    The names are `c0.proto` .. `c<length-1>.proto` because the lane writes the
    same chain as `.proto` files when it runs and compares the two headers byte
    for byte: one schema, two doors.
    """
    lines = []
    for index in range(length):
        lines.append("file {")
        lines.append('  name: "c%d.proto"' % index)
        if index + 1 < length:
            lines.append('  dependency: "c%d.proto"' % (index + 1))
            lines.append("  public_dependency: 0")
        lines.append("}")
    return "\n".join(lines) + "\n"


FIXTURES = (
    # The accepted shape: protoc builds the descriptor itself.
    Fixture.compile("nesting-31",
                    source_text=source_text(ACCEPTED_DEPTH).encode("utf-8")),
    # The refused shape: protoc cannot build it, so the text format does.
    Fixture.craft("nesting-32",
                  source_text=source_text(REFUSED_DEPTH).encode("utf-8"),
                  text=textproto(REFUSED_DEPTH, "nesting-32.proto")
                  .encode("utf-8")),
    # The deep name: a logical name with more components than the limit.
    Fixture.craft("deep-name",
                  text=deep_name_textproto().encode("utf-8")),
    # The dependency chain: protoc accepts it, so it builds the `.pbs` itself.
    Fixture.compile("dependency-chain",
                    source_text=chain_source_text(CHAIN_LENGTH)
                    .encode("utf-8")),
    # The import-chain bound, descriptor half: the deepest accepted chain and
    # the length the recursive walk used to die on. Crafted, because no single
    # `.proto` expresses 1 000 files.
    Fixture.craft("public-chain-1000",
                  text=public_chain_textproto(PUBLIC_CHAIN_ACCEPTED)
                  .encode("utf-8")),
    Fixture.craft("public-chain-20000",
                  text=public_chain_textproto(PUBLIC_CHAIN_REFUSED)
                  .encode("utf-8")),
)


def main():
    args = parse_args(__doc__.split("\n")[0])
    return build_fixtures(HERE, FIXTURES, check=args.check, protoc=args.protoc)


if __name__ == "__main__":
    sys.exit(main())
