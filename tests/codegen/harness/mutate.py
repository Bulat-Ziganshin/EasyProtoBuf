"""Descriptor-set mutator for the B2/M2 discovery slice.

Starts from a protoc-written ``FileDescriptorSet`` (the source file is generated
by ``generate.py`` and compiled with ``protoc --descriptor_set_out
--include_imports``) and rewrites exactly one descriptor field.  The mutation is
recorded together with the descriptor path it touched, which is the C1 coverage
counter for the descriptor half of the generator.

The mutator is structural, not semantic: it parses the wire format into an
ordered field tree, edits it, and writes it back.  Unknown fields survive, so
the only difference between the base set and the mutant is the recorded edit.
It never consults the product or the oracle.

Usage (module API):
    base = compile_base(protoc, work_dir, source_text)
    mutant = apply_operation(base, "reserved-range-overlap")
    mutant.payload, mutant.descriptor_path, mutant.detail
"""

from __future__ import annotations

import dataclasses
import os
import pathlib
import subprocess

import runner


class WireError(ValueError):
    """The bytes are not a protobuf wire-format message."""


# ---------------------------------------------------------------------------
# Wire tree
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class Field:
    number: int
    wire: int
    value: int | bytes
    # A wire-2 field that has been navigated as a message keeps a live child
    # view; encode() serializes the child, so nested edits propagate without an
    # explicit write-back chain.
    child: "Node | None" = None

    def encode(self) -> bytes:
        if self.wire == 0:
            return _varint((self.number << 3) | 0) + _varint(self.value)
        if self.wire == 1:
            return _varint((self.number << 3) | 1) + self.value
        if self.wire == 2:
            raw = self.child.encode() if self.child is not None else self.value
            return (_varint((self.number << 3) | 2) + _varint(len(raw))
                    + raw)
        if self.wire == 5:
            return _varint((self.number << 3) | 5) + self.value
        raise WireError("cannot encode wire type %d" % self.wire)



@dataclasses.dataclass
class Node:
    """One protobuf message as an ordered list of fields."""

    fields: list[Field] = dataclasses.field(default_factory=list)

    def encode(self) -> bytes:
        return b"".join(field.encode() for field in self.fields)

    def all(self, number: int, wire: int | None = None) -> list[Field]:
        return [field for field in self.fields
                if field.number == number and (wire is None or field.wire == wire)]

    def first(self, number: int, wire: int | None = None) -> Field | None:
        matches = self.all(number, wire)
        return matches[0] if matches else None

    def sub(self, number: int) -> "Node | None":
        """Return the first length-delimited field *number* as a live message."""
        field = self.first(number, 2)
        if field is None:
            return None
        if field.child is None:
            field.child = parse(field.value)
        return field.child

    def subs(self, number: int) -> list["Node"]:
        children = []
        for field in self.all(number, 2):
            if field.child is None:
                field.child = parse(field.value)
            children.append(field.child)
        return children

    def text(self, number: int) -> str | None:
        field = self.first(number, 2)
        return field.value.decode("utf-8", "replace") if field is not None else None

    def varint(self, number: int) -> int | None:
        field = self.first(number, 0)
        return int(field.value) if field is not None else None

    def drop(self, number: int, wire: int | None = None, index: int = 0) -> bool:
        matches = [field for field in self.fields
                   if field.number == number and (wire is None or field.wire == wire)]
        if index >= len(matches):
            return False
        self.fields.remove(matches[index])
        return True

    def set_varint(self, number: int, value: int) -> None:
        field = self.first(number, 0)
        if field is None:
            self.fields.append(Field(number, 0, value))
        else:
            field.value = value

    def set_text(self, number: int, value: str) -> None:
        raw = value.encode("utf-8")
        field = self.first(number, 2)
        if field is None:
            self.fields.append(Field(number, 2, raw))
        else:
            field.value = raw

    def add_message(self, number: int, node: "Node") -> None:
        self.fields.append(Field(number, 2, node.encode(), node))

    def add_text(self, number: int, value: str) -> None:
        self.fields.append(Field(number, 2, value.encode("utf-8")))

    def add_varint(self, number: int, value: int) -> None:
        self.fields.append(Field(number, 0, value))


def _varint(value: int) -> bytes:
    if value < 0:
        value += 1 << 64
    out = bytearray()
    while value > 0x7F:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def _read_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while True:
        if offset >= len(data):
            raise WireError("truncated varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7
        if shift > 63:
            raise WireError("varint longer than 64 bits")


def parse(data: bytes) -> Node:
    node = Node()
    offset = 0
    while offset < len(data):
        key, offset = _read_varint(data, offset)
        number, wire = key >> 3, key & 7
        if number == 0:
            raise WireError("field number 0")
        if wire == 0:
            value, offset = _read_varint(data, offset)
        elif wire == 1:
            if offset + 8 > len(data):
                raise WireError("truncated fixed64")
            value, offset = data[offset:offset + 8], offset + 8
        elif wire == 2:
            size, offset = _read_varint(data, offset)
            if offset + size > len(data):
                raise WireError("truncated length-delimited field")
            value, offset = data[offset:offset + size], offset + size
        elif wire == 5:
            if offset + 4 > len(data):
                raise WireError("truncated fixed32")
            value, offset = data[offset:offset + 4], offset + 4
        else:
            raise WireError("unsupported wire type %d" % wire)
        node.fields.append(Field(number, wire, value))
    return node


# ---------------------------------------------------------------------------
# descriptor.proto field numbers used by the operations
# ---------------------------------------------------------------------------

FDS_FILE = 1

FILE_NAME = 1
FILE_MESSAGE = 4
FILE_SERVICE = 6
FILE_SYNTAX = 12

MSG_NAME = 1
MSG_FIELD = 2
MSG_NESTED = 3
MSG_ENUM = 4
MSG_EXTENSION_RANGE = 5
MSG_OPTIONS = 7
MSG_RESERVED_RANGE = 9
MSG_RESERVED_NAME = 10

FIELD_NAME = 1
FIELD_NUMBER = 3
FIELD_LABEL = 4
FIELD_TYPE = 5
FIELD_TYPE_NAME = 6
FIELD_DEFAULT = 7
FIELD_ONEOF_INDEX = 9
FIELD_JSON_NAME = 10

ENUM_VALUE = 2
ENUM_OPTIONS = 3
ENUM_VALUE_NAME = 1
ENUM_VALUE_NUMBER = 2

SERVICE_NAME = 1
SERVICE_METHOD = 2
METHOD_NAME = 1
METHOD_INPUT = 2
METHOD_OUTPUT = 3

MSG_OPTION_MESSAGE_SET = 1
MSG_OPTION_MAP_ENTRY = 7
ENUM_OPTION_ALLOW_ALIAS = 2

TYPE_MESSAGE = 11
TYPE_INT32 = 5
LABEL_REQUIRED = 2

EXTENSION_RANGE_START = 1
EXTENSION_RANGE_END = 2
RESERVED_RANGE_START = 1
RESERVED_RANGE_END = 2


@dataclasses.dataclass
class Mutation:
    operation: str
    descriptor_path: str
    detail: str
    payload: bytes
    target: str


def _file(root: Node) -> Node:
    files = root.subs(FDS_FILE)
    if not files:
        raise WireError("descriptor set has no file")
    return files[0]


def _message(file_node: Node, name: str) -> Node:
    for message in file_node.subs(FILE_MESSAGE):
        if message.text(MSG_NAME) == name:
            return message
    raise WireError("message %s not found" % name)


def _nested(message: Node, name: str) -> Node:
    for nested in message.subs(MSG_NESTED):
        if nested.text(MSG_NAME) == name:
            return nested
    raise WireError("nested message %s not found" % name)


def _field(message: Node, name: str) -> Node:
    for field in message.subs(MSG_FIELD):
        if field.text(FIELD_NAME) == name:
            return field
    raise WireError("field %s not found" % name)


def _target(root: Node) -> str:
    return _file(root).text(FILE_NAME) or "mutant.proto"


# ---------------------------------------------------------------------------
# Operations.  Each one edits exactly one descriptor field and names it.
# ---------------------------------------------------------------------------


def op_reserved_range_overlap(root: Node) -> tuple[str, str]:
    file_node = _file(root)
    message = _message(file_node, "Target")
    field = _field(message, "alpha")
    number = field.varint(FIELD_NUMBER)
    reserved = Node()
    reserved.add_varint(RESERVED_RANGE_START, number)
    reserved.add_varint(RESERVED_RANGE_END, number + 1)
    message.add_message(MSG_RESERVED_RANGE, reserved)
    return (
        "DescriptorProto.reserved_range",
        "reserved range [%d, %d) overlaps field alpha (%d)" % (
            number, number + 1, number),
    )


def op_reserved_name_duplicate(root: Node) -> tuple[str, str]:
    message = _message(_file(root), "Target")
    message.add_text(MSG_RESERVED_NAME, "alpha")
    return (
        "DescriptorProto.reserved_name",
        "reserved_name repeats the declared field name alpha",
    )


def op_extension_range_overflow(root: Node) -> tuple[str, str]:
    message = _message(_file(root), "Target")
    extension = Node()
    extension.add_varint(EXTENSION_RANGE_START, 100)
    extension.add_varint(EXTENSION_RANGE_END, 536870912)
    message.add_message(MSG_EXTENSION_RANGE, extension)
    return (
        "DescriptorProto.ExtensionRange.end",
        "extension range ends at 536870912 (one past max)",
    )


def op_message_set_with_fields(root: Node) -> tuple[str, str]:
    message = _message(_file(root), "Target")
    options = message.sub(MSG_OPTIONS)
    if options is None:
        options = Node()
        message.add_message(MSG_OPTIONS, options)
    options.set_varint(MSG_OPTION_MESSAGE_SET, 1)
    return (
        "DescriptorProto.options.message_set_wire_format",
        "message_set_wire_format = true on a message with fields",
    )


def op_drop_field_name(root: Node) -> tuple[str, str]:
    message = _message(_file(root), "Target")
    field = _field(message, "alpha")
    field.drop(FIELD_NAME)
    return (
        "FieldDescriptorProto.name",
        "field alpha has no name",
    )


def op_clear_field_type(root: Node) -> tuple[str, str]:
    message = _message(_file(root), "Target")
    field = _field(message, "alpha")
    field.drop(FIELD_TYPE)
    return (
        "FieldDescriptorProto.type",
        "field alpha has no declared type",
    )


def op_map_entry_orphan(root: Node) -> tuple[str, str]:
    message = _message(_file(root), "Target")
    nested = _nested(message, "Nested")
    options = nested.sub(MSG_OPTIONS)
    if options is None:
        options = Node()
        nested.add_message(MSG_OPTIONS, options)
    options.set_varint(MSG_OPTION_MAP_ENTRY, 1)
    return (
        "DescriptorProto.options.map_entry",
        "map_entry = true on the ordinary nested message Nested",
    )


def op_map_entry_method(root: Node) -> tuple[str, str]:
    file_node = _file(root)
    message = _message(file_node, "Target")
    entry = None
    for nested in message.subs(MSG_NESTED):
        options = nested.sub(MSG_OPTIONS)
        if options is not None and options.varint(MSG_OPTION_MAP_ENTRY) == 1:
            entry = nested.text(MSG_NAME)
            break
    if entry is None:
        raise WireError("no synthetic map entry in the base set")
    service = file_node.subs(FILE_SERVICE)[0]
    method = service.subs(SERVICE_METHOD)[0]
    method.set_text(METHOD_INPUT, ".demo.Target." + entry)
    return (
        "MethodDescriptorProto.input_type",
        "method endpoint names the synthetic map entry %s" % entry,
    )


def op_json_name_brackets(root: Node) -> tuple[str, str]:
    message = _message(_file(root), "Target")
    field = _field(message, "alpha")
    field.set_text(FIELD_JSON_NAME, "[alpha]")
    return (
        "FieldDescriptorProto.json_name",
        "json_name is shaped like [alpha]",
    )


def op_duplicate_file(root: Node) -> tuple[str, str]:
    file_node = _file(root)
    root.add_message(FDS_FILE, parse(file_node.encode()))
    return (
        "FileDescriptorSet.file",
        "the same FileDescriptorProto is stored twice",
    )


def op_duplicate_field_number(root: Node) -> tuple[str, str]:
    message = _message(_file(root), "Target")
    alpha = _field(message, "alpha")
    beta = _field(message, "beta")
    beta.set_varint(FIELD_NUMBER, alpha.varint(FIELD_NUMBER))
    return (
        "FieldDescriptorProto.number",
        "fields alpha and beta share one number",
    )


def op_missing_dependency(root: Node) -> tuple[str, str]:
    file_node = _file(root)
    file_node.add_text(3, "ghost.proto")
    return (
        "FileDescriptorProto.dependency",
        "dependency names ghost.proto, which is absent from the set",
    )


def op_enum_alias_without_option(root: Node) -> tuple[str, str]:
    message = _message(_file(root), "Target")
    enum = message.subs(MSG_ENUM)[0]
    values = enum.subs(ENUM_VALUE)
    values[1].set_varint(ENUM_VALUE_NUMBER, values[0].varint(ENUM_VALUE_NUMBER))
    return (
        "EnumDescriptorProto.value.number",
        "two enum values share a number without allow_alias",
    )


def op_oneof_index_out_of_range(root: Node) -> tuple[str, str]:
    message = _message(_file(root), "Target")
    field = _field(message, "alpha")
    field.set_varint(FIELD_ONEOF_INDEX, 7)
    return (
        "FieldDescriptorProto.oneof_index",
        "oneof_index 7 names no declared oneof",
    )


def op_field_number_reserved(root: Node) -> tuple[str, str]:
    message = _message(_file(root), "Target")
    field = _field(message, "alpha")
    field.set_varint(FIELD_NUMBER, 19000)
    return (
        "FieldDescriptorProto.number",
        "field number moved into the 19000..19999 implementation range",
    )


def op_map_entry_key_type(root: Node) -> tuple[str, str]:
    message = _message(_file(root), "Target")
    entry = None
    for nested in message.subs(MSG_NESTED):
        options = nested.sub(MSG_OPTIONS)
        if options is not None and options.varint(MSG_OPTION_MAP_ENTRY) == 1:
            entry = nested
            break
    if entry is None:
        raise WireError("no synthetic map entry in the base set")
    key = _field(entry, "key")
    key.set_varint(FIELD_TYPE, TYPE_MESSAGE)
    key.set_text(FIELD_TYPE_NAME, ".demo.Target")
    return (
        "FieldDescriptorProto.type",
        "map entry key changed to a message type",
    )


def op_proto3_required(root: Node) -> tuple[str, str]:
    file_node = _file(root)
    file_node.set_text(FILE_SYNTAX, "proto3")
    message = _message(file_node, "Target")
    alpha = _field(message, "alpha")
    alpha.set_varint(FIELD_LABEL, LABEL_REQUIRED)
    return (
        "FileDescriptorProto.syntax + FieldDescriptorProto.label",
        "proto3 file keeps a required field",
    )


def op_method_missing_endpoint(root: Node) -> tuple[str, str]:
    file_node = _file(root)
    service = file_node.subs(FILE_SERVICE)[0]
    method = service.subs(SERVICE_METHOD)[0]
    method.drop(METHOD_OUTPUT)
    return (
        "MethodDescriptorProto.output_type",
        "method has no output_type",
    )


OPERATIONS = {
    "reserved-range-overlap": (op_reserved_range_overlap, "R3"),
    "reserved-name-duplicate": (op_reserved_name_duplicate, "R3"),
    "extension-range-overflow": (op_extension_range_overflow, "R3"),
    "message-set-with-fields": (op_message_set_with_fields, "R3"),
    "drop-field-name": (op_drop_field_name, "R3"),
    "clear-field-type": (op_clear_field_type, "E4"),
    "map-entry-orphan": (op_map_entry_orphan, "R3"),
    "map-entry-method": (op_map_entry_method, "R6"),
    "json-name-brackets": (op_json_name_brackets, "R6"),
    "duplicate-file": (op_duplicate_file, "E12"),
    "duplicate-field-number": (op_duplicate_field_number, "R3"),
    "missing-dependency": (op_missing_dependency, "R3"),
    "enum-alias-without-option": (op_enum_alias_without_option, "R3"),
    "oneof-index-out-of-range": (op_oneof_index_out_of_range, "R3"),
    "field-number-reserved": (op_field_number_reserved, "R3"),
    "map-entry-key-type": (op_map_entry_key_type, "R3"),
    "proto3-required": (op_proto3_required, "R3"),
    "method-missing-endpoint": (op_method_missing_endpoint, "R3"),
}

OPERATION_ORDER = list(OPERATIONS)


# ---------------------------------------------------------------------------
# Hand-built descriptor sets (``wire.py``)
#
# The operations above start from a set protoc wrote, so they can only reach
# shapes protoc is willing to produce.  These builders write the wire bytes
# directly: a duplicate field number, a missing kind, an editions file.  Each
# carries the triage root or the P1 exception the owner decision assigns.
# ---------------------------------------------------------------------------

import wire  # noqa: E402  (kept below the protoc-written section for clarity)


def _hand(operation: str, descriptor_path: str, detail: str,
          payload: bytes, target: str | None = None) -> Mutation:
    return Mutation(
        operation=operation,
        descriptor_path=descriptor_path,
        detail=detail,
        payload=payload,
        target=target or _target(parse(payload)),
    )


def _simple_file(message: Node | None = None, syntax: str = "proto3",
                 **kwargs) -> bytes:
    messages = [message] if message is not None else []
    return wire.fds([wire.file_descriptor(
        name="root.proto", package="p", syntax=syntax,
        messages=messages, **kwargs)])


def hb_duplicate_field_number() -> Mutation:
    payload = _simple_file(wire.message_type("M", fields=[
        wire.field("a", 1),
        wire.field("b", 1),
    ]))
    return _hand("hb-duplicate-field-number", "FieldDescriptorProto.number",
                 "fields a and b both claim number 1", payload)


def hb_absent_field_type() -> Mutation:
    payload = _simple_file(wire.message_type("M", fields=[
        wire.field("a", 1, type=None),
    ]))
    return _hand("hb-absent-field-type", "FieldDescriptorProto.type",
                 "field a has no declared kind", payload)


def hb_group_type() -> Mutation:
    group = wire.message_type("G", fields=[wire.field("x", 2)])
    payload = _simple_file(wire.message_type(
        "M",
        fields=[wire.field("g", 1, type=wire.TYPE_GROUP,
                           type_name=".p.M.G")],
        nested=[group]), syntax="proto2")
    return _hand("hb-group-type", "FieldDescriptorProto.type",
                 "field g is a proto2 group", payload)


def hb_unknown_field_type() -> Mutation:
    payload = _simple_file(wire.message_type("M", fields=[
        wire.field("a", 1, type=99),
    ]))
    return _hand("hb-unknown-field-type", "FieldDescriptorProto.type",
                 "field a declares kind 99, which descriptor.proto does not define",
                 payload)


def hb_field_number_zero() -> Mutation:
    payload = _simple_file(wire.message_type("M", fields=[
        wire.field("a", 0),
    ]))
    return _hand("hb-field-number-zero", "FieldDescriptorProto.number",
                 "field a claims number 0", payload)


def hb_message_set_with_fields() -> Mutation:
    payload = _simple_file(wire.message_type(
        "M", fields=[wire.field("a", 1)],
        message_set_wire_format=True), syntax="proto2")
    return _hand("hb-message-set-with-fields",
                 "DescriptorProto.options.message_set_wire_format",
                 "message_set_wire_format is true on a message with fields",
                 payload)


def hb_reserved_range_overlap() -> Mutation:
    payload = _simple_file(wire.message_type(
        "M", fields=[wire.field("a", 1)], reserved_ranges=[(1, 2)]))
    return _hand("hb-reserved-range-overlap",
                 "DescriptorProto.reserved_range",
                 "reserved range [1, 2) contains field a", payload)


def hb_extension_range_overflow() -> Mutation:
    payload = _simple_file(wire.message_type(
        "M", extension_ranges=[(1, 600000000)]), syntax="proto2")
    return _hand("hb-extension-range-overflow",
                 "DescriptorProto.ExtensionRange.end",
                 "extension range ends at 600000000", payload)


def hb_map_entry_unreferenced() -> Mutation:
    entry = wire.message_type(
        "ItemsEntry",
        fields=[wire.field("key", 1, type=wire.TYPE_BYTES),
                wire.field("value", 2, type=wire.TYPE_INT32)],
        map_entry=True)
    payload = _simple_file(wire.message_type("M", nested=[entry]))
    return _hand("hb-map-entry-unreferenced",
                 "DescriptorProto.options.map_entry",
                 "an unreferenced synthetic map entry has a bytes key", payload)


def hb_public_index_duplicate() -> Mutation:
    payload = wire.fds([
        wire.file_descriptor(name="dep.proto", package="d", syntax="proto3"),
        wire.file_descriptor(name="root.proto", package="p", syntax="proto3",
                             dependencies=["dep.proto"],
                             public_dependencies=[0, 0]),
    ])
    return _hand("hb-public-index-duplicate",
                 "FileDescriptorProto.public_dependency",
                 "public_dependency repeats index 0", payload,
                 target="root.proto")


def hb_editions_file() -> Mutation:
    payload = wire.fds([wire.file_descriptor(
        name="root.proto", package="p", syntax="editions", edition=1000)])
    return _hand("hb-editions-file", "FileDescriptorProto.edition",
                 "the file declares edition 2023", payload)


def hb_method_map_entry_endpoint() -> Mutation:
    entry = wire.message_type(
        "ItemsEntry",
        fields=[wire.field("key", 1, type=wire.TYPE_STRING),
                wire.field("value", 2, type=wire.TYPE_INT32)],
        map_entry=True)
    holder = wire.message_type(
        "Holder", fields=[wire.field("items", 1, type=wire.TYPE_MESSAGE,
                                     type_name=".p.Holder.ItemsEntry",
                                     label=wire.LABEL_REPEATED)],
        nested=[entry])
    payload = _simple_file(holder, services=[wire.service("S", [
        wire.method("Call", ".p.Holder.ItemsEntry", ".p.Holder")])])
    return _hand("hb-method-map-entry-endpoint", "MethodDescriptorProto.input_type",
                 "a method endpoint names a synthetic map entry", payload)


def hb_json_name_brackets() -> Mutation:
    payload = _simple_file(wire.message_type("M", fields=[
        wire.field("a", 1, json_name="[a]"),
    ]))
    return _hand("hb-json-name-brackets", "FieldDescriptorProto.json_name",
                 "json_name is shaped like [a]", payload)


def hb_duplicate_file() -> Mutation:
    file_payload = wire.file_descriptor(
        name="root.proto", package="p", syntax="proto3",
        messages=[wire.message_type("M", fields=[wire.field("a", 1)])])
    payload = wire.fds([file_payload, file_payload])
    return _hand("hb-duplicate-file", "FileDescriptorSet.file",
                 "the same FileDescriptorProto is stored twice", payload)


def hb_proto3_required() -> Mutation:
    payload = _simple_file(wire.message_type("M", fields=[
        wire.field("a", 1, label=wire.LABEL_REQUIRED),
    ]))
    return _hand("hb-proto3-required",
                 "FileDescriptorProto.syntax + FieldDescriptorProto.label",
                 "a proto3 file keeps a required field", payload)


# shape -> (builder, expected, authority, direction).  ``expected`` is the
# verdict the owner decision predicts:
#   * ``parity`` — both tools refuse;
#   * ``exception`` — codegen refuses, protoc accepts, and the refusal carries
#     the marker of the P1 exception named by ``authority``;
#   * ``known-root`` — the tools diverge in the named direction:
#     ``more-permissive`` (codegen accepts, protoc refuses) or ``stricter``
#     (codegen refuses, protoc accepts), and the triage root is ``authority``.
# The plan probe measured each shape against protoc 30.1 and the pinned codegen
# binary before these values were fixed; ``handbuilt.py`` re-checks them on
# every run, so a change of direction or diagnostic is a new finding.
HAND_BUILT = {
    "hb-duplicate-field-number": (hb_duplicate_field_number, "parity", None, None),
    "hb-absent-field-type": (hb_absent_field_type, "exception", "E4", "stricter"),
    "hb-group-type": (hb_group_type, "exception", "E4", "stricter"),
    "hb-unknown-field-type": (hb_unknown_field_type, "exception", "E4", "stricter"),
    "hb-field-number-zero": (hb_field_number_zero, "parity", None, None),
    # Step B5 closed root R3 for these three shapes: the descriptor model now
    # decodes the reserved declarations and the MessageSet option, and the
    # extension-range sweep enforces the protocol maximum, so codegen refuses
    # exactly where protoc refuses. `handbuilt.py` re-checks that on every run.
    "hb-message-set-with-fields": (hb_message_set_with_fields, "parity", None,
                                   None),
    "hb-reserved-range-overlap": (hb_reserved_range_overlap, "parity", None,
                                  None),
    "hb-extension-range-overflow": (hb_extension_range_overflow, "parity", None,
                                    None),
    "hb-map-entry-unreferenced": (hb_map_entry_unreferenced, "exception", "E10",
                                  "stricter"),
    "hb-public-index-duplicate": (hb_public_index_duplicate, "exception", "E9",
                                  "stricter"),
    "hb-editions-file": (hb_editions_file, "exception", "E8", "stricter"),
    # Step B8 closed root R6 for these two shapes: a method endpoint may name a
    # synthetic map entry (protoc's ownership rule is a field rule) and a
    # bracket-shaped custom `json_name` is refused, so both tools agree now.
    "hb-method-map-entry-endpoint": (hb_method_map_entry_endpoint, "parity",
                                     None, None),
    "hb-json-name-brackets": (hb_json_name_brackets, "parity", None, None),
    "hb-proto3-required": (hb_proto3_required, "parity", None, None),
}

HAND_BUILT_ORDER = list(HAND_BUILT)


def hand_built(shape: str) -> Mutation:
    if shape not in HAND_BUILT:
        raise KeyError(shape)
    builder, _expected, _authority, _direction = HAND_BUILT[shape]
    return builder()


def hand_built_target(shape: str) -> str:
    return hand_built(shape).target



# ---------------------------------------------------------------------------
# Base compilation
# ---------------------------------------------------------------------------

BASE_SOURCE = """\
syntax = "proto2";
package demo;

message Target
{
    optional int32 alpha = 1;
    optional string beta = 2 [default = "x"];
    repeated int64 gamma = 3;
    optional Nested delta = 4;
    optional Corge corge = 5;
    map<string, int32> scores = 6;

    message Nested
    {
        optional bool ok = 1;
    }

    enum Corge
    {
        A = 0;
        B = 1;
    }
}

message Other
{
    optional int32 value = 1;
}

service Svc
{
    rpc Call (Target) returns (Other);
}
"""

BASE_LOGICAL_NAME = "mutant.proto"


def compile_base(protoc: pathlib.Path, work_dir: pathlib.Path,
                 source_text: str = BASE_SOURCE) -> bytes:
    """Write the base source and return the protoc-written descriptor set."""
    work_dir.mkdir(parents=True, exist_ok=True)
    source = work_dir / BASE_LOGICAL_NAME
    source.write_text(source_text, encoding="utf-8", newline="\n")
    descriptor = work_dir / "base.pbs"
    done = subprocess.run(
        [str(protoc), "-I", str(work_dir),
         "--descriptor_set_out=" + str(descriptor), "--include_imports",
         str(source)],
        cwd=str(work_dir), capture_output=True, shell=False, timeout=60,
    )
    if done.returncode != 0:
        raise WireError("base compilation failed: %s" % (
            done.stderr.decode("utf-8", "replace").strip().splitlines()[:1]))
    return descriptor.read_bytes()


def apply_operation(base: bytes, operation: str) -> Mutation:
    if operation not in OPERATIONS:
        raise KeyError(operation)
    function, _root = OPERATIONS[operation]
    root = parse(base)
    descriptor_path, detail = function(root)
    return Mutation(
        operation=operation,
        descriptor_path=descriptor_path,
        detail=detail,
        payload=root.encode(),
        target=_target(root),
    )


def base_target(base: bytes) -> str:
    return _target(parse(base))


if __name__ == "__main__":
    import argparse
    import sys

    parser = argparse.ArgumentParser(description="descriptor-set mutator self-check")
    parser.add_argument("--protoc", type=pathlib.Path,
                        default=pathlib.Path(os.environ.get("EASYPB_PROTOC", "protoc")))
    parser.add_argument("--work", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent /
                        "generated" / "_mutate-selftest")
    args = parser.parse_args()
    args.protoc = runner.resolve_tool(args.protoc)
    base = compile_base(args.protoc, args.work)
    for name in OPERATION_ORDER:
        mutation = apply_operation(base, name)
        if mutation.payload == base:
            print("unchanged: %s" % name)
            raise SystemExit(1)
        # The mutated set must still be parseable by this module.
        parse(mutation.payload)
        print("%-30s %-55s %d bytes" % (
            name, mutation.descriptor_path, len(mutation.payload)))
    print("base target: %s" % base_target(base))
    raise SystemExit(0)
