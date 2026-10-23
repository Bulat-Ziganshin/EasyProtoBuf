"""Rebuild the crafted `.pbs` fixtures of the CLI lane (S4).

`anon.pbs`, `external-anon.pbs`, `casepair.pbs`, `bad-anon.pbs`,
`anon-two.pbs` and `mixed-anon.pbs` cannot be produced by
`protoc`: the unnamed ones carry no `FileDescriptorProto.name` (protoc always
writes one), and `casepair.pbs` is a deliberately case-only pair of logical
names. They are encoded here with the same minimal wire writer the plan-5
review probes used, so the bytes are reproducible without a protobuf runtime.

The `.proto` fixtures beside this file are ordinary source and are the source
half of the same lane; this script writes only the `.pbs` files.

Usage (from the repository root):

    python tests/codegen/cli/data/make_fixtures.py
"""

import io
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# descriptor.proto field numbers used by these fixtures.
FILE_DESCRIPTOR_PROTO_NAME = 1
FILE_DESCRIPTOR_PROTO_PACKAGE = 2
FILE_DESCRIPTOR_PROTO_MESSAGE_TYPE = 4
FILE_DESCRIPTOR_PROTO_SYNTAX = 12
DESCRIPTOR_PROTO_NAME = 1
DESCRIPTOR_PROTO_FIELD = 2
FIELD_DESCRIPTOR_PROTO_NAME = 1
FIELD_DESCRIPTOR_PROTO_NUMBER = 3
FIELD_DESCRIPTOR_PROTO_LABEL = 4
FIELD_DESCRIPTOR_PROTO_TYPE = 5
FIELD_DESCRIPTOR_PROTO_TYPE_NAME = 6
FILE_DESCRIPTOR_SET_FILE = 1

LABEL_OPTIONAL = 1
TYPE_INT32 = 5
TYPE_STRING = 9
TYPE_MESSAGE = 11


def varint(value):
    out = b""
    while value > 0x7F:
        out += bytes([(value & 0x7F) | 0x80])
        value >>= 7
    return out + bytes([value])


def tag(field, wire):
    return varint((field << 3) | wire)


def s(field, text):
    raw = text.encode("utf-8")
    return tag(field, 2) + varint(len(raw)) + raw


def m(field, payload):
    return tag(field, 2) + varint(len(payload)) + payload


def scalar_field(name, number, kind=TYPE_INT32):
    return (s(FIELD_DESCRIPTOR_PROTO_NAME, name) +
            tag(FIELD_DESCRIPTOR_PROTO_NUMBER, 0) + varint(number) +
            tag(FIELD_DESCRIPTOR_PROTO_LABEL, 0) + varint(LABEL_OPTIONAL) +
            tag(FIELD_DESCRIPTOR_PROTO_TYPE, 0) + varint(kind))


def message_field(name, number, type_name):
    return (scalar_field(name, number, TYPE_MESSAGE) +
            s(FIELD_DESCRIPTOR_PROTO_TYPE_NAME, type_name))


def message(name, fields):
    body = s(DESCRIPTOR_PROTO_NAME, name)
    for field in fields:
        body += m(DESCRIPTOR_PROTO_FIELD, field)
    return body


def file_proto(name, package, message_name, fields, syntax="proto3"):
    body = b""
    if name is not None:
        body += s(FILE_DESCRIPTOR_PROTO_NAME, name)
    if package:
        body += s(FILE_DESCRIPTOR_PROTO_PACKAGE, package)
    body += m(FILE_DESCRIPTOR_PROTO_MESSAGE_TYPE,
              message(message_name, fields))
    body += s(FILE_DESCRIPTOR_PROTO_SYNTAX, syntax)
    return body


def write(path, payload):
    with io.open(os.path.join(HERE, path), "wb") as handle:
        handle.write(payload)


# W-18: one file with no logical name and no external reference. The
# container path must not become a selectable target.
write("anon.pbs",
      m(FILE_DESCRIPTOR_SET_FILE,
        file_proto(None, "an", "M", [scalar_field("v", 1)])))

# W-14: the same anonymous shape, but the message field is an unresolved
# external type, so the compatibility link records its one warning.
write("external-anon.pbs",
      m(FILE_DESCRIPTOR_SET_FILE,
        file_proto(None, "", "M",
                   [message_field("f", 1, ".ext.Y")])))

# W-26: two named files that differ only in case. Both declare one message
# with the same field, so the headers differ only in the message name.
write("casepair.pbs",
      m(FILE_DESCRIPTOR_SET_FILE,
        file_proto("api/x.proto", "api", "Lower", [scalar_field("v", 1)])) +
      m(FILE_DESCRIPTOR_SET_FILE,
        file_proto("api/X.proto", "api", "Upper", [scalar_field("v", 1)])))

# W-19: one unnamed file whose message declares field number 1 twice. The
# semantic pass refuses it, and the diagnostic must label the file by its
# container (the name is absent on purpose, so there is nothing else to use).
write("bad-anon.pbs",
      m(FILE_DESCRIPTOR_SET_FILE,
        file_proto(None, "", "M",
                   [scalar_field("a", 1, TYPE_STRING),
                    scalar_field("b", 1, TYPE_STRING)])))

# W-19: two unnamed files. The loader refuses the shape, and the container
# path must print once (before the fix it printed twice).
write("anon-two.pbs",
      m(FILE_DESCRIPTOR_SET_FILE,
        file_proto(None, "", "M", [scalar_field("a", 1, TYPE_STRING)])) +
      m(FILE_DESCRIPTOR_SET_FILE,
        file_proto(None, "", "M", [scalar_field("a", 1, TYPE_STRING)])))

# W-19: one named and one unnamed file. The same refusal and the same
# one-path rule as `anon-two.pbs`.
write("mixed-anon.pbs",
      m(FILE_DESCRIPTOR_SET_FILE,
        file_proto("mixed-named.proto", "mx", "Named",
                   [scalar_field("v", 1)])) +
      m(FILE_DESCRIPTOR_SET_FILE,
        file_proto(None, "mx", "Anon", [scalar_field("v", 1)])))

print("wrote anon.pbs, external-anon.pbs, casepair.pbs, bad-anon.pbs, "
      "anon-two.pbs and mixed-anon.pbs")
