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
  `ext-ok`, `ext-max`, `message-set-empty`, `w68-dep`, `w68-root`, and the R04
  accepted controls `opt-jstype-int64`, `opt-lazy-message`) are compiled with
  `--descriptor_set_out`. `w68-root` is included even though its import is
  missing from the SET: protoc emits only the listed file, which is exactly
  the thin descriptor W-68 is about;
- the shapes protoc refuses to build from source - duplicate method names, a
  method endpoint that does not exist, a reversed map entry, a proto3 enum
  whose first value is not zero, a proto3 or field-overlapping extension
  range, the W-36 containers, and the R04 invalid `jstype`/`lazy`/extension
  declaration shapes - are the lane's `.txtpb` files; the helper encodes every
  `.txtpb` beside the table with `--encode=google.protobuf.FileDescriptorSet`.
  The text format does not validate them; it only encodes the descriptor the
  fixture names. `opt-extrange-declaration` is crafted even though protoc
  accepts its source half: `descriptor.proto` marks `declaration` and
  `verification` `[retention = RETENTION_SOURCE]`, so a protoc-written
  descriptor carries neither.

One accepted R04 shape needs a byte the text format will not spell:
`opt-jstype-unknown` declares an enumerator of the closed `FieldOptions.JSType`
enum that descriptor.proto does not define (7), which protoc's decoder routes
to the unknown fields. The fixture is encoded twice - once with `JS_STRING`
and once with `JS_NUMBER`, whose encodings differ in exactly the one value
byte - and 7 is written into that byte (the `field-type-unknown` pattern).

The S08 wire rows are the RAW fixtures: an undeclared `edition` enumerator,
100 and 101 nested unknown groups, and a mid-message tag with field number 0.
None of them can be spelled in the text format (a closed enum is validated,
and the format has no unknown fields), so `common.descriptor_wire` writes the
bytes directly. All of them declare the same logical file, so the accepted
ones must produce one header and one print.

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

from common.descriptor_fixtures import (Fixture, build_fixtures, fail,
                                        encode_textproto, parse_args, write)
from common.descriptor_wire import descriptor_wire_set

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
    # V-15: an accepted schema whose reserved names/ranges and extension range
    # must appear in the source print exactly as in protoc's `.pbs` print.
    Fixture.compile("retained-ranges"),
    # V-42: a proto2 message with all three labels, so the source and
    # descriptor doors agree on every label the source grammar can spell.
    Fixture.compile("labels-ok"),
    Fixture.compile("w68-dep"),
    Fixture.compile("w68-root"),
    # R04 accepted controls: a legal `jstype`, a legal `lazy`, and an extension
    # declaration list protoc accepts.
    Fixture.compile("opt-jstype-int64"),
    Fixture.compile("opt-lazy-message"),
)

# The one R04 fixture whose single byte the text format cannot spell: an
# enumerator outside descriptor.proto's closed JSType enum.
PATCHED_JSTYPE_FIXTURE = "opt-jstype-unknown"
PATCHED_JSTYPE_VALUE = 7
PATCHED_JSTYPE_PROBES = ("JS_STRING", "JS_NUMBER")


def patched_jstype_bytes(protoc, descriptor_dir, text):
    """The encoding of `text` with the modeled enumerator replaced by 7.

    The text format refuses an enumerator it does not know, so the fixture is
    encoded with two declared enumerators whose value bytes are both one-byte
    varints; they differ in exactly that byte, which is where 7 is written. The
    assertion is the point: a second difference would make the patch a guess.
    """
    encodings = []
    decoded = text.decode("utf-8")
    for probe in PATCHED_JSTYPE_PROBES:
        replaced = decoded.replace("jstype: JS_NORMAL", "jstype: %s" % probe)
        if replaced == decoded:
            fail("%s.txtpb does not declare 'jstype: JS_NORMAL'"
                 % PATCHED_JSTYPE_FIXTURE)
        encodings.append(encode_textproto(protoc, descriptor_dir,
                                           replaced.encode("utf-8")))
    first, second = encodings
    if len(first) != len(second):
        fail("the two encodings of %s differ in length" % PATCHED_JSTYPE_FIXTURE)
    differences = [index for index in range(len(first))
                   if first[index] != second[index]]
    if len(differences) != 1:
        fail("the two encodings of %s differ in %d bytes, expected exactly one"
             % (PATCHED_JSTYPE_FIXTURE, len(differences)))
    index = differences[0]
    return first[:index] + bytes([PATCHED_JSTYPE_VALUE]) + first[index + 1:]


# S08 (V-26, V-28, V-41): the wire-level decode shapes the text format cannot
# carry. `descriptor-wire-base`, `descriptor-wire-edition-unknown` and
# `descriptor-wire-groups-100` differ only in unknown bytes, so they must
# generate one header and print one tree; the 101-level nest and the zero tag
# are refused before generation.
RAW_FIXTURES = (
    ("descriptor-wire-base", descriptor_wire_set()),
    ("descriptor-wire-edition-unknown", descriptor_wire_set(edition=99)),
    ("descriptor-wire-groups-100", descriptor_wire_set(unknown_groups=100)),
    ("descriptor-wire-groups-101", descriptor_wire_set(unknown_groups=101)),
    ("descriptor-wire-tag-zero", descriptor_wire_set(tag_zero=True)),
)


def main():
    args = parse_args(__doc__.split("\n")[0])
    for stem, data in RAW_FIXTURES:
        write(os.path.join(HERE, stem + ".pbs"), data, args.check)
    return build_fixtures(HERE, FIXTURES,
                          patches={PATCHED_JSTYPE_FIXTURE:
                                   patched_jstype_bytes},
                          check=args.check, protoc=args.protoc)


if __name__ == "__main__":
    sys.exit(main())
