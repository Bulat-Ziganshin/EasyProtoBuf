"""Minimal protobuf *writer* for hand-built descriptor sets (step B2b).

The descriptor mutator starts from a set ``protoc`` wrote, so it can only reach
shapes the reference compiler is willing to produce.  The untrusted descriptor
path must also survive shapes ``protoc`` refuses to write: a duplicate field
number, a missing field kind, an editions file, a repeated public index.  This
module writes those bytes directly.

Field numbers are ``descriptor.proto``'s (protobuf 30.1); the names used here
mirror a subset of it and are deliberately not validated.  Nothing in this
module consults the product or the oracle: a shape is built once, byte for
byte, so the case is reproducible.

Usage:
    from wire import wire

    payload = wire.fds([wire.file(
        name="root.proto", syntax="proto3", package="p",
        messages=[wire.message("M", fields=[wire.field("a", 1, type=5)])],
    )])

This is the file review's ``build/code-review-1023-ds/schema-descriptor/wire.py``
generalized: it keeps the same length-delimited/varint helpers and adds the
message builders the hand-built plan needs.
"""

from __future__ import annotations

# ---------------------------------------------------------------------------
# Wire primitives
# ---------------------------------------------------------------------------


def varint(value: int) -> bytes:
    if value < 0:
        value += 1 << 64
    out = bytearray()
    while value > 0x7F:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def tag(field: int, wire_type: int) -> bytes:
    return varint((field << 3) | wire_type)


def encode(value: str | bytes) -> bytes:
    return value.encode("utf-8") if isinstance(value, str) else bytes(value)


def string(field: int, value: str | bytes) -> bytes:
    raw = encode(value)
    return tag(field, 2) + varint(len(raw)) + raw


def message(field: int, payload: bytes) -> bytes:
    return tag(field, 2) + varint(len(payload)) + payload


def varint_field(field: int, value: int) -> bytes:
    return tag(field, 0) + varint(value)


# ---------------------------------------------------------------------------
# FieldDescriptorProto.Type and Label
# ---------------------------------------------------------------------------

TYPE_DOUBLE = 1
TYPE_FLOAT = 2
TYPE_INT64 = 3
TYPE_UINT64 = 4
TYPE_INT32 = 5
TYPE_FIXED64 = 6
TYPE_FIXED32 = 7
TYPE_BOOL = 8
TYPE_STRING = 9
TYPE_GROUP = 10
TYPE_MESSAGE = 11
TYPE_BYTES = 12
TYPE_UINT32 = 13
TYPE_ENUM = 14
TYPE_SFIXED32 = 15
TYPE_SFIXED64 = 16
TYPE_SINT32 = 17
TYPE_SINT64 = 18

LABEL_OPTIONAL = 1
LABEL_REQUIRED = 2
LABEL_REPEATED = 3


# ---------------------------------------------------------------------------
# descriptor.proto messages
# ---------------------------------------------------------------------------


def field(
    name: str | None = None,
    number: int | None = None,
    type: int | None = TYPE_INT32,
    label: int | None = LABEL_OPTIONAL,
    type_name: str | None = None,
    default: str | None = None,
    oneof_index: int | None = None,
    json_name: str | None = None,
    packed: bool | None = None,
    proto3_optional: bool | None = None,
    extra: bytes = b"",
) -> bytes:
    """One ``FieldDescriptorProto``.

    A ``None`` argument omits the field, which is how a shape with an absent
    field kind is built; the defaults produce an ordinary optional int32.
    """
    body = b""
    if name is not None:
        body += string(1, name)
    if number is not None:
        body += varint_field(3, number)
    if label is not None:
        body += varint_field(4, label)
    if type is not None:
        body += varint_field(5, type)
    if type_name is not None:
        body += string(6, type_name)
    if default is not None:
        body += string(7, default)
    if packed is not None:
        body += message(8, varint_field(2, 1 if packed else 0))
    if oneof_index is not None:
        body += varint_field(9, oneof_index)
    if json_name is not None:
        body += string(10, json_name)
    if proto3_optional is not None:
        body += varint_field(17, 1 if proto3_optional else 0)
    return body + extra


def oneof(name: str) -> bytes:
    return string(1, name)


def enum_value(name: str, number: int) -> bytes:
    return string(1, name) + varint_field(2, number)


def enum(
    name: str | None,
    values: list[tuple[str, int]],
    allow_alias: bool | None = None,
    reserved_ranges: list[tuple[int, int]] | None = None,
    reserved_names: list[str] | None = None,
) -> bytes:
    body = string(1, name) if name is not None else b""
    for value_name, value_number in values:
        body += message(2, enum_value(value_name, value_number))
    if allow_alias is not None:
        body += message(3, varint_field(2, 1 if allow_alias else 0))
    for start, end in reserved_ranges or []:
        body += message(4, varint_field(1, start) + varint_field(2, end))
    for reserved in reserved_names or []:
        body += string(5, reserved)
    return body


def extension_range(start: int, end: int) -> bytes:
    return varint_field(1, start) + varint_field(2, end)


def reserved_range(start: int, end: int) -> bytes:
    return varint_field(1, start) + varint_field(2, end)


def message_type(
    name: str | None,
    fields: list[bytes] | None = None,
    nested: list[bytes] | None = None,
    enums: list[bytes] | None = None,
    oneofs: list[str] | None = None,
    extension_ranges: list[tuple[int, int]] | None = None,
    extension_fields: list[bytes] | None = None,
    reserved_ranges: list[tuple[int, int]] | None = None,
    reserved_names: list[str] | None = None,
    map_entry: bool | None = None,
    message_set_wire_format: bool | None = None,
    extra: bytes = b"",
) -> bytes:
    """One ``DescriptorProto``.

    Named ``message_type`` because ``message`` is the wire helper above.
    """
    body = string(1, name) if name is not None else b""
    for item in fields or []:
        body += message_wrap(2, item)
    for item in nested or []:
        body += message_wrap(3, item)
    for item in enums or []:
        body += message_wrap(4, item)
    for start, end in extension_ranges or []:
        body += message_wrap(5, extension_range(start, end))
    for item in extension_fields or []:
        body += message_wrap(6, item)
    options = b""
    if message_set_wire_format is not None:
        options += varint_field(1, 1 if message_set_wire_format else 0)
    if map_entry is not None:
        options += varint_field(7, 1 if map_entry else 0)
    if options:
        body += message_wrap(7, options)
    for item in oneofs or []:
        body += message_wrap(8, oneof(item))
    for start, end in reserved_ranges or []:
        body += message_wrap(9, reserved_range(start, end))
    for item in reserved_names or []:
        body += string(10, item)
    return body + extra


def method(
    name: str,
    input_type: str,
    output_type: str,
    client_streaming: bool | None = None,
    server_streaming: bool | None = None,
) -> bytes:
    body = string(1, name) + string(2, input_type) + string(3, output_type)
    if client_streaming is not None:
        body += varint_field(5, 1 if client_streaming else 0)
    if server_streaming is not None:
        body += varint_field(6, 1 if server_streaming else 0)
    return body


def service(name: str, methods: list[bytes]) -> bytes:
    body = string(1, name)
    for item in methods:
        body += message_wrap(2, item)
    return body


def file_descriptor(
    name: str | None = None,
    package: str | None = None,
    messages: list[bytes] | None = None,
    enums: list[bytes] | None = None,
    services: list[bytes] | None = None,
    syntax: str | None = None,
    edition: int | None = None,
    dependencies: list[str] | None = None,
    public_dependencies: list[int] | None = None,
    weak_dependencies: list[int] | None = None,
    extra: bytes = b"",
) -> bytes:
    body = b""
    if name is not None:
        body += string(1, name)
    if package is not None:
        body += string(2, package)
    for item in dependencies or []:
        body += string(3, item)
    for item in messages or []:
        body += message_wrap(4, item)
    for item in enums or []:
        body += message_wrap(5, item)
    for item in services or []:
        body += message_wrap(6, item)
    for index in public_dependencies or []:
        body += varint_field(10, index)
    for index in weak_dependencies or []:
        body += varint_field(11, index)
    if syntax is not None:
        body += string(12, syntax)
    if edition is not None:
        body += varint_field(14, edition)
    return body + extra


def fds(files: list[bytes], extra: bytes = b"") -> bytes:
    body = b""
    for item in files:
        body += message_wrap(1, item)
    return body + extra


# ``message`` is the generic length-delimited helper; alias it under a name the
# descriptor builders can use without shadowing a message type.
message_wrap = message
