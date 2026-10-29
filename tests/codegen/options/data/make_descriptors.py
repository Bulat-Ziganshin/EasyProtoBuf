#!/usr/bin/env python3
"""make_descriptors.py - rebuild the options lane's descriptor fixtures.

The lane was source-only before step S13. The V-20 pin needs a `.pbs` because
the source option table deliberately has no `features` row: `FileOptions` is
one of the option messages the decoder skips, so only the descriptor door can
show that an opaque `FeatureSet` payload has no effect.

Three fixtures are built:

- `feature-set-control.pbs` - the option-less half, encoded from its `.txtpb`;
- `feature-set-opaque.pbs` - the same logical file with a well-formed
  `FeatureSet` carrying non-default edition features, encoded from its
  `.txtpb`;
- `feature-set-opaque-malformed.pbs` - the same logical file whose
  `FileOptions` carries a `FeatureSet` payload that is not a decodable
  message. The text format cannot spell that shape, so the bytes are written
  directly from `descriptor.proto`'s field numbers.

All three declare `feature-set-opaque.proto`, so the three generated headers
must be byte-identical.

Usage:

    python tests/codegen/options/data/make_descriptors.py [--protoc PATH] [--check]
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# `tests/codegen`, so `common.*` is importable from anywhere.
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
# Do not leave an untracked `common/__pycache__` in the repository.
sys.dont_write_bytecode = True

from common.descriptor_fixtures import (Fixture, build_fixtures, parse_args,
                                        write)
from common.descriptor_wire import message_field, tag, text_field, varint

FILE_NAME = "feature-set-opaque.proto"

# FileDescriptorProto field numbers used by the raw fixture:
#   name = 1, package = 2, message_type = 4, options = 8, syntax = 12
# DescriptorProto.name = 1, field = 2; FieldDescriptorProto.name = 1,
# number = 3, label = 4, type = 5; FileOptions.features = 50.
LABEL_OPTIONAL = 1
TYPE_INT32 = 5

FIXTURE_TABLE = (
    Fixture.craft("feature-set-control"),
    Fixture.craft("feature-set-opaque"),
)


def malformed_options_set():
    """The set whose FileOptions.features is not a decodable FeatureSet.

    One invalid tag (`0x0f`: field 1, wire type 7) is the whole FeatureSet
    payload. The FileDescriptorProto -> FileOptions -> features length frames
    are all well-formed, so the descriptor decoder can skip the field; protoc,
    which decodes FileOptions and then FeatureSet, refuses it.
    """
    field = text_field(1, "value")
    field += tag(3, 0) + varint(1)               # number
    field += tag(4, 0) + varint(LABEL_OPTIONAL)  # label
    field += tag(5, 0) + varint(TYPE_INT32)      # type
    message = text_field(1, "Payload") + message_field(2, field)
    options = message_field(50, bytes([0x0F]))
    file = text_field(1, FILE_NAME)
    file += text_field(2, "opaque")
    file += text_field(12, "proto2")
    file += message_field(4, message)
    file += message_field(8, options)
    return message_field(1, file)


def main():
    args = parse_args(__doc__.split("\n")[0])
    write(os.path.join(HERE, "feature-set-opaque-malformed.pbs"),
          malformed_options_set(), args.check)
    return build_fixtures(HERE, table=FIXTURE_TABLE, check=args.check,
                          protoc=args.protoc)


if __name__ == "__main__":
    sys.exit(main())
