#!/usr/bin/env python3
"""descriptor_wire.py - raw `.pbs` bytes for wire shapes the text format cannot spell.

The parity and validity lanes ship committed `.pbs` fixtures built by their
`data/make_descriptors.py`. Most of them are protoc's own encoding of a
`.txtpb` or `.proto` source. The three shapes of step S08 are not writable in
the text format at all:

- an `edition` enumerator the pinned `descriptor.proto` does not declare (the
  text format validates a closed enum and answers "Unknown enumeration value");
- 100 and 101 nested unknown groups (the text format has no unknown fields);
- a tag whose field number is 0 in the middle of a message (ditto).

This module writes those bytes directly from `descriptor.proto`'s field
numbers, so the fixture depends on neither the product decoder nor the oracle.
Every builder produces one logical file (`descriptor-wire.proto`) with one
message and one optional int32 field. The accepted shapes differ only in the
unknown bytes the decoder must ignore, so their generated headers and prints
are byte-identical and the lane can compare them with each other.
"""

# Field numbers used below, from the pinned descriptor.proto:
#   FileDescriptorSet.file = 1
#   FileDescriptorProto.name = 1, package = 2, message_type = 4, edition = 14
#   DescriptorProto.name = 1, field = 2
#   FieldDescriptorProto.name = 1, number = 3, label = 4, type = 5
FILE_NAME = "descriptor-wire.proto"

# FieldDescriptorProto labels and kinds used by the one field.
LABEL_OPTIONAL = 1
TYPE_INT32 = 5


def varint(value):
    """The wire encoding of a non-negative integer."""
    out = bytearray()
    while value > 0x7F:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def tag(field, wire_type):
    """The wire encoding of one field tag."""
    return varint((field << 3) | wire_type)


def text_field(field, value):
    """A length-delimited UTF-8 field."""
    payload = value.encode("utf-8")
    return tag(field, 2) + varint(len(payload)) + payload


def message_field(field, payload):
    """A length-delimited field carrying a nested message."""
    return tag(field, 2) + varint(len(payload)) + payload


def descriptor_wire_set(edition=None, unknown_groups=0, tag_zero=False):
    """One FileDescriptorSet with the accepted control or one wire edge case.

    `edition` writes FileDescriptorProto.edition (wire field 14) with that
    value; None omits the field. `unknown_groups` writes that many nested
    START_GROUP/END_GROUP pairs as an undeclared FileDescriptorProto field
    (15), which the decoder skips. `tag_zero` writes a tag with field number 0
    and a varint payload between the name and the package: the mid-message
    shape V-41 is about.
    """
    field = text_field(1, "a")
    field += tag(3, 0) + varint(1)               # number
    field += tag(4, 0) + varint(LABEL_OPTIONAL)  # label
    field += tag(5, 0) + varint(TYPE_INT32)      # type
    message = text_field(1, "M") + message_field(2, field)

    file = text_field(1, FILE_NAME)
    if edition is not None:
        file += tag(14, 0) + varint(edition)
    if tag_zero:
        file += tag(0, 0) + varint(0)
    for _ in range(unknown_groups):
        file += tag(15, 3)
    for _ in range(unknown_groups):
        file += tag(15, 4)
    file += text_field(2, "p")
    file += message_field(4, message)
    return message_field(1, file)
