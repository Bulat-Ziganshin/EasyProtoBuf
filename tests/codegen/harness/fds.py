"""Minimal read-only FileDescriptorSet inspection for the harness.

The harness needs two facts from a `.pbs` fixture before it can invoke either
tool: the logical file name to use as `--target`, and the dependency graph that
tells whether the set is self-contained.  This module parses only the wire
format; it is not a descriptor model and never validates anything.
"""

from __future__ import annotations

from dataclasses import dataclass, field


class WireError(ValueError):
    """The input is not a protobuf wire-format message."""


def _varint(data: bytes, offset: int) -> tuple[int, int]:
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


def _fields(data: bytes):
    """Yield ``(field_number, wire_type, value)``.

    Length-delimited values are bytes, varints are ints, fixed32/fixed64 are
    bytes, and groups are skipped (their nested fields are not surfaced).
    """
    offset = 0
    while offset < len(data):
        key, offset = _varint(data, offset)
        number, wire = key >> 3, key & 7
        if number == 0:
            raise WireError("field number 0")
        if wire == 0:
            value, offset = _varint(data, offset)
            yield number, wire, value
        elif wire == 1:
            if offset + 8 > len(data):
                raise WireError("truncated fixed64")
            yield number, wire, data[offset:offset + 8]
            offset += 8
        elif wire == 2:
            size, offset = _varint(data, offset)
            if offset + size > len(data):
                raise WireError("truncated length-delimited field")
            yield number, wire, data[offset:offset + size]
            offset += size
        elif wire == 3:
            # Start group: skip until the matching end group (wire 4) with the
            # same field number.  descriptor.proto has no group fields, so this
            # only keeps a crafted fixture parseable instead of failing.
            depth = 1
            while depth:
                if offset >= len(data):
                    raise WireError("unterminated group")
                inner_key, offset = _varint(data, offset)
                inner_number, inner_wire = inner_key >> 3, inner_key & 7
                if inner_wire == 3:
                    depth += 1
                elif inner_wire == 4:
                    if inner_number != number:
                        raise WireError("mismatched end group")
                    depth -= 1
                elif inner_wire == 0:
                    _, offset = _varint(data, offset)
                elif inner_wire == 1:
                    offset += 8
                elif inner_wire == 2:
                    size, offset = _varint(data, offset)
                    offset += size
                elif inner_wire == 5:
                    offset += 4
                else:
                    raise WireError("invalid wire type %d in group" % inner_wire)
                if offset > len(data):
                    raise WireError("truncated group")
        elif wire == 4:
            raise WireError("unexpected end group")
        elif wire == 5:
            if offset + 4 > len(data):
                raise WireError("truncated fixed32")
            yield number, wire, data[offset:offset + 4]
            offset += 4
        else:
            raise WireError("invalid wire type %d" % wire)


@dataclass
class FileDescriptorInfo:
    name: str | None = None
    dependencies: list[str] = field(default_factory=list)
    public_dependencies: list[int] = field(default_factory=list)
    weak_dependencies: list[int] = field(default_factory=list)
    syntax: str | None = None
    has_package: bool = False
    package: str = ""


def _text(value: bytes) -> str:
    return value.decode("utf-8", "replace")


def file_descriptors(data: bytes) -> list[FileDescriptorInfo]:
    """Return the files of a FileDescriptorSet in stored order."""
    files: list[FileDescriptorInfo] = []
    for number, wire, payload in _fields(data):
        if number != 1 or wire != 2:
            continue
        info = FileDescriptorInfo()
        for inner, inner_wire, value in _fields(payload):
            if inner == 1 and inner_wire == 2:
                info.name = _text(value)
            elif inner == 2 and inner_wire == 2:
                info.has_package = True
                info.package = _text(value)
            elif inner == 3 and inner_wire == 2:
                info.dependencies.append(_text(value))
            elif inner == 10 and inner_wire == 0:
                info.public_dependencies.append(value)
            elif inner == 11 and inner_wire == 0:
                info.weak_dependencies.append(value)
            elif inner == 12 and inner_wire == 2:
                info.syntax = _text(value)
        files.append(info)
    return files


def select_target(files: list[FileDescriptorInfo],
                  preferred: str | None = None) -> str | None:
    """Pick the file to use as ``--target``.

    The last file that no other file depends on is the usual root of a
    protoc-written set; otherwise the last named file is the best guess.  A set
    with no names has no target and both tools should refuse it.  ``preferred``
    names the conventional root (``root.proto``) of a multi-root parity set.
    """
    named = [f for f in files if f.name]
    if not named:
        return None
    if preferred and any(f.name == preferred for f in named):
        return preferred
    depended_on = {dep for f in files for dep in f.dependencies}
    roots = [f.name for f in named if f.name not in depended_on]
    if roots:
        return roots[-1]
    return named[-1].name


def header_stem(logical_name: str) -> str:
    """Return the generated header stem for a logical .proto name."""
    base = logical_name.replace("\\", "/").rsplit("/", 1)[-1]
    if base.endswith(".proto"):
        base = base[: -len(".proto")]
    return base
