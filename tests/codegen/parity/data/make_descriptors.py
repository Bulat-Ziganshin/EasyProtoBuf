#!/usr/bin/env python3
"""make_descriptors.py - rebuild the parity lane's `.pbs` fixtures.

The lane drives two input paths, so every rule it checks exists twice: as
`.proto` source text and as a `.pbs` descriptor set of the same shape. This
script writes the descriptor halves and is the only way they are produced, so
a fixture cannot drift away from the shape it claims to have.

The mechanics - the protoc lookup, the `descriptor.proto` lookup, the
text-format encoder, the writer and the `--check` gate - live in
`tests/codegen/common/descriptor_fixtures.py`; this script is the lane's table
of fixtures.

Two kinds of fixtures:

- the shapes protoc itself accepts (`methods-ok`, `map-ok`, `enum-ok`,
  `ext-ok`, `ext-max`, `message-set-empty`, `w68-dep`, `w68-root`) are compiled
  with `--descriptor_set_out`. `w68-root` is included even though its import is
  missing from the SET: protoc emits only the listed file, which is exactly
  the thin descriptor W-68 is about;
- the shapes protoc refuses to build from source - duplicate method names, a
  method endpoint that does not exist, a reversed map entry, a proto3 enum
  whose first value is not zero, a proto3 or field-overlapping extension
  range, and the W-36 containers - are the lane's `.txtpb` files; the helper
  encodes every `.txtpb` beside the table with
  `--encode=google.protobuf.FileDescriptorSet`. The text format does not
  validate them; it only encodes the descriptor the fixture names.

Usage:

    python tests/codegen/parity/data/make_descriptors.py [--protoc PATH] [--check]

`--check` rebuilds every fixture in memory and compares it with the committed
file instead of writing it.
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

# The source fixtures protoc accepts. The others are intentional refusals and
# have no compiled half: their `.pbs` twin is encoded from the `.txtpb` beside
# it (or, for W-36, the containers themselves are the fixture).
FIXTURES = (
    Fixture.compile("methods-ok"),
    Fixture.compile("map-ok"),
    Fixture.compile("enum-ok"),
    Fixture.compile("ext-ok"),
    Fixture.compile("ext-max"),
    Fixture.compile("message-set-empty"),
    Fixture.compile("w68-dep"),
    Fixture.compile("w68-root"),
)


def main():
    args = parse_args(__doc__.split("\n")[0])
    return build_fixtures(HERE, FIXTURES, check=args.check, protoc=args.protoc)


if __name__ == "__main__":
    sys.exit(main())
