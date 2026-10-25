#!/usr/bin/env python3
"""make_descriptors.py - rebuild the validity lane's `.pbs` fixtures.

The lane drives two input paths, so every fixture exists twice: as `.proto`
source text and as a `.pbs` descriptor set. This script writes the descriptor
halves and is the only way they are produced, so a fixture can never drift
away from the shape it claims to have.

The mechanics - the protoc lookup, the `descriptor.proto` lookup, the
text-format encoder, the writer and the `--check` gate - live in
`tests/codegen/common/descriptor_fixtures.py`. This script is the lane's set
of exceptions to the helper's defaults.

Two shapes come from protoc:

- the paired fixtures (`defaults-*.proto`) are compiled with
  `--descriptor_set_out`, so the descriptor is protoc's own encoding of the
  source file and carries the same `default_value` spelling protoc writes;
- the crafted fixtures (`*.txtpb`) are encoded with
  `--encode=google.protobuf.FileDescriptorSet`, because protoc cannot produce
  them from source text: an enum value named `1A`, a field whose type is 99, a
  field with no type at all, a proto2 group, a file name with a newline, a raw
  out-of-range or leading-plus default, the W-42/W-43 collision shapes, and the
  B7 `allow_alias` shapes whose rules live in protoc's source parser only.

A crafted `.pbs` wins over the `.proto` of the same stem: the raw spelling is
the point of `defaults-float-overflow`, `defaults-bytes-upper-hex`,
`plus-int-descriptor` and their siblings, so the helper must not replace it
with protoc's canonical form. The source-only fixtures in
`SOURCE_ONLY_FIXTURES` are reported and never compiled.

ONE crafted shape needs one byte that protoc's text format will not carry:
`field-type-unknown` declares `type: 99`, and the text format answers
"Unknown enumeration value of \"99\" for field \"type\"". `patched_kind_bytes`
therefore encodes that fixture TWICE - once with `type: TYPE_INT32` and once
with `type: TYPE_SINT32`, both one-byte varints - asserts that the two
encodings differ in exactly one byte, and writes 99 into that byte. The result
is protoc's own encoding of the same descriptor with the unknown kind.

Usage:

    python tests/codegen/validity/data/make_descriptors.py [--protoc PATH] [--check]

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

from common.descriptor_fixtures import (build_fixtures,
                                        encode_textproto, fail, parse_args)

# The source-only fixtures: protoc refuses most of them, and a generated `.pbs`
# would either fail the script or add an unintended descriptor twin. The
# validity lane drives these from the `.proto` directly (W-15, W-40, W-41, B7).
SOURCE_ONLY_FIXTURES = (
    "bom",
    "bom-control",
    "empty-syntax",
    "grammar-allow-alias-false",
    "grammar-allow-alias-false-aliased",
    "grammar-allow-alias-useless",
    "grammar-enum-json-name",
    "grammar-field-option-plus",
    "grammar-leading-semicolon",
    "grammar-option-plus",
    "grammar-option-plus-known",
    "grammar-repeated-default",
    "json-name-map-collision",
    "literals-aggregate-nul",
    "literals-nested-comment",
    "literals-octal-08",
    "literals-octal-09",
    "literals-octal-enum",
    "plus-enum-value",
    "plus-float-default",
    "plus-inf-nan",
    "plus-int-default",
)

# The one fixture whose text protoc will not parse, and the two legal type
# values it is encoded with instead (both are one-byte varints, so the two
# encodings differ in exactly one byte - the kind itself).
PATCHED_TYPE_FIXTURE = "field-type-unknown"
PATCHED_TYPE_VALUE = 99
PATCHED_TYPE_PROBES = ("TYPE_INT32", "TYPE_SINT32")


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


PATCHES = {PATCHED_TYPE_FIXTURE: patched_kind_bytes}


def main():
    args = parse_args(__doc__.split("\n")[0])
    return build_fixtures(HERE, source_only=SOURCE_ONLY_FIXTURES,
                          patches=PATCHES, check=args.check,
                          protoc=args.protoc)


if __name__ == "__main__":
    sys.exit(main())
