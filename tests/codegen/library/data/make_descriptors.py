#!/usr/bin/env python3
"""make_descriptors.py - rebuild the library lane's `.pbs` fixtures.

The lane drives the C++ API and the CLI over the same descriptor sets. Every
binary fixture is produced here and nowhere else, so a fixture cannot drift
away from the shape it claims to have. The lane itself never calls protoc: the
`.pbs` files are committed.

The mechanics - the protoc lookup, the `descriptor.proto` lookup, the
text-format encoder, the writer and the `--check` gate - live in
`tests/codegen/common/descriptor_fixtures.py`; this script is the lane's table
of fixtures.

Two kinds of fixtures:

- the shapes protoc accepts (`options-default`, `w47-external`,
  `w47-external-enum-default`, `w48-wire`, the P7 pair) are compiled with
  `--descriptor_set_out`. `w47-external` and `w47-external-enum-default` are
  emitted WITHOUT `--include_imports`, so each is the thin descriptor W-47 is
  about: the root file names its dependency but the set does not carry it;
- the shapes protoc refuses to build from source - a message field with a
  default, and an enum default that is not an identifier - are the lane's
  `.txtpb` files (`w47-external-message-default`, `w47-external-enum-1a`); the
  helper encodes every `.txtpb` beside the table with
  `--encode=google.protobuf.FileDescriptorSet`. The text format does not
  validate them; it only encodes the descriptor the fixture names.

The P7 pair is the CR-08 thin/full triple: the `.proto` root compiled WITHOUT
`--include_imports` (the thin compatibility set) and WITH it (the complete
closure the header must equal).

Usage:

    python tests/codegen/library/data/make_descriptors.py [--protoc PATH] [--check]

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

# The external enum the P7 pair imports, beside the root file.
P7_EXT = os.path.join(HERE, "p7-ext")

# (descriptor-set stem, source stem, extra include dirs, extra protoc flags).
# The two stems differ only for the enum control, whose output is named for its
# role and whose source names the shape it declares.
FIXTURES = (
    Fixture.compile("options-default"),
    Fixture.compile("w47-external"),
    Fixture.compile("w47-external-enum-default", source="w47-external-enum"),
    Fixture.compile("w48-wire"),
    Fixture.compile("p7-thin", source="b", include_dirs=(P7_EXT,)),
    Fixture.compile("p7-full", source="b", include_dirs=(P7_EXT,),
                    flags=("--include_imports",)),
)


def main():
    args = parse_args(__doc__.split("\n")[0])
    return build_fixtures(HERE, FIXTURES, check=args.check, protoc=args.protoc)


if __name__ == "__main__":
    sys.exit(main())
