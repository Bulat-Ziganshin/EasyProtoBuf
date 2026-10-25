#!/usr/bin/env python3
"""make_descriptors.py - rebuild the names lane's `.pbs` fixtures.

The lane drives two input paths, so every fixture exists twice: as `.proto`
source text and as a `.pbs` descriptor set. This script writes the descriptor
halves and is the only way they are produced, so a fixture can never drift
away from the shape it claims to have.

The mechanics - the protoc lookup, the `descriptor.proto` lookup, the
text-format encoder, the writer and the `--check` gate - live in
`tests/codegen/common/descriptor_fixtures.py`. This script has no fixture
table: the helper compiles every `.proto` here with `--descriptor_set_out` and
encodes every `.txtpb` with `--encode=google.protobuf.FileDescriptorSet`, and a
`.txtpb` wins over the `.proto` of the same stem.

FIVE fixtures are crafted, because no `.proto` text can produce them. Four
declare a descriptor-origin name that is not a C++ identifier (`0Zc`, `0Xb`,
`0M`, `0E`): the source lexer cannot spell a digit-first name, so protoc
cannot compile a `.proto` into that descriptor, although protoc accepts such a
descriptor set. The fifth, `escape-enum-value`, declares the two enumerators
`class` and `class_`, and the reference compiler refuses that schema outright
("Enum name class_ has the same name as class if you ignore case and strip out
the enum name prefix"), so its descriptor cannot come from
`--descriptor_set_out` either. Every other `.pbs` is protoc's own encoding of
the `.proto` beside it.

Usage:

    python tests/codegen/names/data/make_descriptors.py [--protoc PATH] [--check]

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

from common.descriptor_fixtures import build_fixtures, parse_args


def main():
    args = parse_args(__doc__.split("\n")[0])
    return build_fixtures(HERE, check=args.check, protoc=args.protoc)


if __name__ == "__main__":
    sys.exit(main())
