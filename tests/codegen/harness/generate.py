"""Seeded grammar generator for the B2/M2 discovery slice.

Deterministic by construction: every case is built from ``(seed, case index)``
by a SplitMix64 stream, so the same seed produces byte-identical inputs on any
run.  Each case records the rule it exercises and the family it belongs to; the
discovery driver classifies a divergence against that record.

Modes:

* ``valid`` — a valid-by-construction source tree.  The oracle (protoc 30.1)
  must accept it; a rejection is a generator bug, not a finding.
* ``source-mutation`` — one lexical or grammar rule is broken at a time.  The
  oracle may accept or reject; the mutation is a probe, not an assertion.
* ``descriptor-mutation`` — a protoc-written descriptor set with one wire field
  rewritten by ``mutate.py``.

The catalogue deliberately covers the mechanisms named by the triage roots
R1–R6 and the P1 exception list E1–E6.  R7–R10 are CLI/print/resource
properties and are outside P1/P2; the generator does not fake them.

Usage:
    python tests/codegen/harness/generate.py --seed 1 --check
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import pathlib
import sys

import mutate
import runner

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_PROTOC = pathlib.Path(os.environ.get("EASYPB_PROTOC", "protoc"))
GEN_ROOT = HERE / "generated"

SCALARS = [
    "double", "float", "int32", "int64", "uint32", "uint64", "sint32",
    "sint64", "fixed32", "fixed64", "sfixed32", "sfixed64", "bool", "string",
    "bytes",
]
PACKABLE = [item for item in SCALARS if item not in ("string", "bytes")]
MAP_KEYS = [
    "int32", "int64", "uint32", "uint64", "sint32", "sint64", "fixed32",
    "fixed64", "sfixed32", "sfixed64", "bool", "string",
]
SAFE_NUMBERS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
                19, 20, 100, 101, 102, 1000, 1001, 2047, 2048, 262143, 262144,
                536870911]
DEFAULT_INTS = ["0", "1", "-1", "2147483647", "-2147483648", "010", "0x1F",
                "0x7fffffff", "42", "-0777"]
DEFAULT_UINTS = ["0", "1", "4294967295", "0777", "0xFFFFFFFF", "42"]
DEFAULT_LONGS = ["0", "1", "-1", "9223372036854775807", "-9223372036854775808",
                 "010", "0x7FFFFFFFFFFFFFFF", "-0777"]
DEFAULT_ULONGS = ["0", "1", "18446744073709551615", "010",
                  "0xFFFFFFFFFFFFFFFF", "42"]
DEFAULT_FLOATS = ["0", "-0.0", "1.5", "-2.25", "1e10", "1e-10", "inf", "-inf",
                  "nan", "010", "0755", "3.141592653589793"]
DEFAULT_STRINGS = [
    '"plain"', '"a\\tb"', '"a\\nb"', '"\\101\\102"', '"\\x41\\x7f"',
    '"\\377"', '"\\001\\002"', '"\\u00e9"', '"quote\\"inside"',
    '"back\\\\slash"', '""',
]
DEFAULT_BYTES = ['"\\000\\377\\x80"', '"abc"', '"\\x00"', '""', '"\\001"']


class Rng:
    """SplitMix64; the sequence is stable across Python versions."""

    MASK = (1 << 64) - 1

    def __init__(self, seed: int) -> None:
        self.state = seed & self.MASK

    def u64(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & self.MASK
        value = self.state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & self.MASK
        value = ((value ^ (value >> 27)) * 0x94D049BB123111EB) & self.MASK
        return value ^ (value >> 31)

    def below(self, limit: int) -> int:
        return self.u64() % limit

    def pick(self, values):
        return values[self.below(len(values))]

    def chance(self, percent: int) -> bool:
        return self.below(100) < percent

    def shuffled(self, values):
        items = list(values)
        for index in range(len(items) - 1, 0, -1):
            other = self.below(index + 1)
            items[index], items[other] = items[other], items[index]
        return items


@dataclasses.dataclass
class Case:
    id: str
    order: int
    mode: str
    family: str
    files: dict[str, bytes]
    target: str
    mechanism: str
    root: str | None
    exception: str | None
    oracle_expect: str  # accept | reject | any
    detail: str
    coverage: list[str]
    descriptor_path: str | None = None


def _source(text: str) -> bytes:
    return text.encode("utf-8")


def _case(index: int, mode: str, family: str, mechanism: str,
          files: dict[str, bytes], target: str, root: str | None,
          oracle_expect: str, detail: str, coverage: list[str],
          exception: str | None = None,
          descriptor_path: str | None = None) -> Case:
    return Case(
        id="m2-%03d-%s" % (index, family),
        order=index,
        mode=mode,
        family=family,
        files=files,
        target=target,
        mechanism=mechanism,
        root=root,
        exception=exception,
        oracle_expect=oracle_expect,
        detail=detail,
        coverage=sorted(set(coverage)),
        descriptor_path=descriptor_path,
    )


def _header(package: str | None, syntax: str = "proto2",
            file_options: list[str] | None = None) -> list[str]:
    lines = ['syntax = "%s";' % syntax]
    if package:
        lines.append("package %s;" % package)
    for option in file_options or []:
        lines.append("option %s;" % option)
    lines.append("")
    return lines


def _take_numbers(rng: Rng, count: int) -> list[int]:
    numbers = rng.shuffled(SAFE_NUMBERS)
    if count > len(numbers):
        raise RuntimeError("not enough safe field numbers")
    return numbers[:count]


# ---------------------------------------------------------------------------
# Valid families
# ---------------------------------------------------------------------------


def valid_scalars2(rng: Rng, index: int) -> Case:
    name = "Msg%d" % index
    numbers = _take_numbers(rng, 24)
    lines = _header("pkg%d" % rng.below(40))
    lines.append("message %s" % name)
    lines.append("{")
    lines.append("    required uint32 id = %d;" % numbers[0])
    for offset, scalar in enumerate(SCALARS, start=1):
        lines.append("    optional %s f_%s = %d;" % (
            scalar, scalar, numbers[offset]))
    lines.append("    repeated int64 packed_values = %d [packed = true];"
                 % numbers[17])
    lines.append("    repeated string names = %d;" % numbers[18])
    lines.append("    optional Inner inner = %d;" % numbers[19])
    lines.append("    optional Kind kind = %d [default = NEG];" % numbers[20])
    lines.append("    repeated Kind kinds = %d [packed = true];" % numbers[21])
    lines.append("    option deprecated = true;")
    lines.append("    message Inner")
    lines.append("    {")
    lines.append("        optional int32 x = 1;")
    lines.append("        optional string label = 2 [default = \"inner\"];")
    lines.append("    }")
    lines.append("    enum Kind")
    lines.append("    {")
    lines.append("        ZERO = 0;")
    lines.append("        ONE = 1;")
    lines.append("        NEG = -1;")
    lines.append("    }")
    lines.append("}")
    return _case(
        index, "valid", "scalars2", "valid-scalars2",
        {"case.proto": _source("\n".join(lines) + "\n")}, "case.proto", None,
        "accept", "all scalar types, labels, nested message and enum",
        ["syntax.proto2", "package", "message", "field.optional",
         "field.required", "field.repeated", "scalar.all", "message.nested",
         "enum.nested", "enum.negative", "option.packed", "option.deprecated",
         "default.enum"],
    )


def valid_scalars3(rng: Rng, index: int) -> Case:
    name = "M3_%d" % index
    numbers = _take_numbers(rng, 21)
    lines = _header("p3pkg%d" % rng.below(40), "proto3")
    lines.append("message %s" % name)
    lines.append("{")
    for offset, scalar in enumerate(SCALARS):
        lines.append("    %s f_%s = %d;" % (scalar, scalar, numbers[offset]))
    lines.append("    repeated int32 packed_off = %d [packed = false];"
                 % numbers[15])
    lines.append("    repeated Kind kinds = %d;" % numbers[16])
    lines.append("    map<string, int32> scores = %d;" % numbers[17])
    lines.append("    map<int64, Inner> entries = %d;" % numbers[18])
    lines.append("    oneof choice")
    lines.append("    {")
    lines.append("        string text = %d;" % numbers[19])
    lines.append("        Inner nested = %d;" % numbers[20])
    lines.append("    }")
    lines.append("    message Inner { bool ok = 1; }")
    lines.append("    enum Kind { KIND_UNSPECIFIED = 0; KIND_ONE = 1; }")
    lines.append("}")
    return _case(
        index, "valid", "scalars3", "valid-scalars3",
        {"case.proto": _source("\n".join(lines) + "\n")}, "case.proto", None,
        "accept", "proto3 singular fields, maps, oneof and nested types",
        ["syntax.proto3", "package", "message", "field.singular", "scalar.all",
         "map", "oneof", "message.nested", "enum.nested", "option.packed",
         "field.number.boundary"],
    )


def valid_defaults(rng: Rng, index: int) -> Case:
    numbers = _take_numbers(rng, 24)
    fields = [
        ("int32", "i32", rng.pick(DEFAULT_INTS)),
        ("int64", "i64", rng.pick(DEFAULT_LONGS)),
        ("uint32", "u32", rng.pick(DEFAULT_UINTS)),
        ("uint64", "u64", rng.pick(DEFAULT_ULONGS)),
        ("sint32", "s32", rng.pick(DEFAULT_INTS)),
        ("sint64", "s64", rng.pick(DEFAULT_LONGS)),
        ("fixed32", "f32", rng.pick(DEFAULT_UINTS)),
        ("fixed64", "f64", rng.pick(DEFAULT_ULONGS)),
        ("sfixed32", "sf32", rng.pick(DEFAULT_INTS)),
        ("sfixed64", "sf64", rng.pick(DEFAULT_LONGS)),
        ("float", "flt", rng.pick(DEFAULT_FLOATS)),
        ("double", "dbl", rng.pick(DEFAULT_FLOATS)),
        ("bool", "flag", rng.pick(["true", "false"])),
        ("string", "text", rng.pick(DEFAULT_STRINGS)),
        ("bytes", "blob", rng.pick(DEFAULT_BYTES)),
        ("Kind", "kind", rng.pick(["ZERO", "ONE", "NEG"])),
    ]
    lines = _header(
        "defs%d" % rng.below(40),
        file_options=[
            'java_package = "com.example.defs%d"' % index,
            "optimize_for = SPEED",
            "cc_enable_arenas = true",
            'go_package = "example.com/defs%d"' % index,
        ],
    )
    lines.append("message Defaults%d" % index)
    lines.append("{")
    for offset, (type_name, field_name, literal) in enumerate(fields):
        lines.append("    optional %s %s = %d [default = %s];" % (
            type_name, field_name, numbers[offset], literal))
    lines.append("    repeated int32 packed_values = %d [packed = true];"
                 % numbers[16])
    lines.append("    optional int32 renamed = %d [json_name = \"renamedValue\","
                 " deprecated = true];" % numbers[17])
    lines.append("    enum Kind { ZERO = 0; ONE = 1; NEG = -1; }")
    lines.append("}")
    return _case(
        index, "valid", "defaults", "valid-defaults",
        {"case.proto": _source("\n".join(lines) + "\n")}, "case.proto", None,
        "accept", "every scalar default spelling and boundary value",
        ["syntax.proto2", "package", "message", "default.int32", "default.int64",
         "default.uint32", "default.uint64", "default.sint32", "default.sint64",
         "default.fixed32", "default.fixed64", "default.sfixed32",
         "default.sfixed64", "default.float", "default.double", "default.bool",
         "default.string", "default.bytes", "default.enum", "default.octal",
         "default.hex", "default.inf", "default.nan", "default.escape",
         "default.unicode", "option.packed", "option.json_name",
         "option.deprecated", "option.java_package", "option.optimize_for",
         "option.cc_enable_arenas", "option.go_package"],
    )


def valid_names(rng: Rng, index: int) -> Case:
    keyword_pool = [
        "operator", "delete", "inline", "public", "private", "virtual",
        "using", "new", "this", "friend", "typename",
    ]
    message = rng.pick(keyword_pool)
    field = rng.pick([item for item in keyword_pool if item != message])
    lines = _header("names%d" % index)
    lines.append("message %s" % message)
    lines.append("{")
    lines.append("    optional int32 %s = 1;" % field)
    lines.append("    optional int32 std = 2;")
    lines.append("    optional int32 easypb = 3;")
    lines.append("    optional int32 encode = 4;")
    lines.append("    optional int32 decode = 5;")
    lines.append("    optional int32 _leading = 6;")
    lines.append("    optional int32 double__underscore = 7;")
    lines.append("    optional int32 with1digit = 8;")
    lines.append("    optional int32 foo = 9;")
    lines.append("    optional int32 Foo = 10;")
    lines.append("    enum Values { class = 0; template = 1; namespace = 2; }")
    lines.append("    optional Values values = 11;")
    lines.append("}")
    return _case(
        index, "valid", "names", "valid-names",
        {"case.proto": _source("\n".join(lines) + "\n")}, "case.proto", None,
        "accept", "C++ keywords, codec/global names, leader and double underscore",
        ["syntax.proto2", "package", "message", "field.keyword",
         "enum.keyword", "name.underscore", "name.digit", "name.case-only"],
    )


def valid_policy_names(rng: Rng, index: int) -> Case:
    variant = index % 2
    lines = _header("pol%d" % index)
    if variant == 0:
        lines.append("message Collide%d" % index)
        lines.append("{")
        lines.append("    optional int32 foo = 1;")
        lines.append("    optional bool has_foo = 2;")
        lines.append("}")
        mechanism = "policy-presence-flag"
        detail = "has_foo is the presence flag of foo"
    else:
        lines.append("message Escaped%d" % index)
        lines.append("{")
        lines.append("    optional int32 class = 1;")
        lines.append("    optional int32 class_ = 2;")
        lines.append("}")
        mechanism = "policy-name-escape"
        detail = "class and class_ escape to the same C++ name"
    return _case(
        index, "valid", "policy-names", mechanism,
        {"case.proto": _source("\n".join(lines) + "\n")}, "case.proto", None,
        "accept", detail,
        ["syntax.proto2", "message", "field.keyword", "name.collision"],
    )


def valid_enums(rng: Rng, index: int) -> Case:
    syntax = "proto3" if index % 3 == 0 else "proto2"
    lines = _header("enums%d" % index, syntax)
    if syntax == "proto3":
        lines.append("enum Top { option allow_alias = true; "
                     "TOP_UNSPECIFIED = 0; TOP_ONE = 1; "
                     "TOP_ALIAS = 1 [deprecated = true]; }")
        lines.append("enum Colors { option allow_alias = true; "
                     "COLOR_UNSPECIFIED = 0; COLOR_RED = 1; COLOR_CRIMSON = 1; }")
        lines.append("message Holder { Colors color = 1; Top top = 2; }")
        lines.append("enum Valued { option allow_alias = true; A = 0; B = 1; C = 1; }")
    else:
        lines.append("enum Top { option allow_alias = true; NEG = -2; ZERO = 0; "
                     "ONE = 1; UNO = 1 [deprecated = true]; }")
        lines.append("enum Colors { reserved 2, 5, 9 to 11; "
                     "reserved \"GREEN\", \"BLUE\"; RED = 0; CRIMSON = 1; }")
        lines.append("message Holder { optional Colors color = 1 [default = CRIMSON]; "
                     "optional Top top = 2 [default = NEG]; }")
    lines.append("message Outer%d" % index)
    lines.append("{")
    lines.append("    enum Nested { NESTED_ZERO = 0; NESTED_NEG = -1; }")
    lines.append("    optional Nested nested = 1;")
    lines.append("}")
    return _case(
        index, "valid", "enums", "valid-enums",
        {"case.proto": _source("\n".join(lines) + "\n")}, "case.proto", None,
        "accept", "enum aliases, reserved, negative values, defaults and nesting",
        ["syntax." + syntax, "enum", "enum.alias", "enum.reserved",
         "enum.negative", "enum.nested", "default.enum", "option.allow_alias",
         "option.deprecated"],
    )


def valid_imports(rng: Rng, index: int) -> Case:
    variant = index % 4
    files: dict[str, bytes] = {}
    if variant == 0:
        files["common.proto"] = _source(
            'syntax = "proto2";\npackage common;\n'
            "message Common { optional int32 v = 1; }\n"
            "enum Color { COLOR_RED = 0; COLOR_BLUE = 1; }\n"
        )
        main = _header("app%d" % index)
        main.append('import "common.proto";')
        main.append("message Main { optional common.Common c = 1; "
                    "optional common.Color color = 2 [default = COLOR_BLUE]; }")
        mechanism = "import-plain"
        coverage = ["import", "import.plain", "package.multi"]
    elif variant == 1:
        files["base.proto"] = _source(
            'syntax = "proto2";\npackage base;\n'
            "message Base { optional int32 v = 1; }\n"
        )
        files["common.proto"] = _source(
            'syntax = "proto2";\npackage common;\n'
            'import public "base.proto";\n'
            "message Common { optional base.Base b = 1; }\n"
        )
        main = _header("app%d" % index)
        main.append('import "common.proto";')
        main.append("message Main { optional common.Common c = 1; "
                    "optional base.Base b = 2; }")
        mechanism = "import-public"
        coverage = ["import", "import.public", "import.transitive", "package.multi"]
    elif variant == 2:
        files["common.proto"] = _source(
            'syntax = "proto2";\npackage common;\n'
            "message Common { optional int32 v = 1; }\n"
        )
        main = _header("app%d" % index)
        main.append('import weak "common.proto";')
        main.append("message Main { optional common.Common c = 1; }")
        mechanism = "import-weak"
        coverage = ["import", "import.weak"]
    else:
        files["common.proto"] = _source(
            'syntax = "proto2";\npackage common;\n'
            "message Common { optional int32 v = 1; }\n"
        )
        files["a.proto"] = _source(
            'syntax = "proto2";\npackage a;\n'
            'import "common.proto";\n'
            "message A { optional common.Common c = 1; }\n"
        )
        files["b.proto"] = _source(
            'syntax = "proto2";\npackage b;\n'
            'import "common.proto";\n'
            "message B { optional common.Common c = 1; }\n"
        )
        main = _header("app%d" % index)
        main.append('import "a.proto";')
        main.append('import "b.proto";')
        main.append("message Main { optional a.A a = 1; optional b.B b = 2; }")
        mechanism = "import-diamond"
        coverage = ["import", "import.diamond", "package.multi"]
    files["main.proto"] = _source("\n".join(main) + "\n")
    return _case(
        index, "valid", "imports", mechanism, files, "main.proto", None,
        "accept", "import graph variant %d" % variant,
        coverage + ["syntax.proto2", "package", "message", "default.enum"],
    )


def valid_services(rng: Rng, index: int) -> Case:
    lines = _header("svc%d" % index)
    lines.append("message Request { optional int32 id = 1; "
                 "message Nested { optional string text = 1; } }")
    lines.append("message Response { optional bytes payload = 1; }")
    lines.append("service Service%d" % index)
    lines.append("{")
    lines.append("    rpc Get (Request) returns (Response);")
    lines.append("    rpc Put (Request.Nested) returns (Response);")
    lines.append("    rpc Echo (Request) returns (Request) { option deprecated = true; }")
    lines.append("}")
    return _case(
        index, "valid", "services", "valid-services",
        {"case.proto": _source("\n".join(lines) + "\n")}, "case.proto", None,
        "accept", "unary service with scalar, nested and message endpoints",
        ["syntax.proto2", "package", "message", "message.nested", "service",
         "method.unary", "option.deprecated"],
    )


def valid_maps_oneof(rng: Rng, index: int) -> Case:
    keys = rng.shuffled(MAP_KEYS)
    numbers = _take_numbers(rng, 20)
    lines = _header("maps%d" % index)
    lines.append("message Maps%d" % index)
    lines.append("{")
    lines.append("    message Value { optional int32 x = 1; }")
    lines.append("    enum Kind { ZERO = 0; ONE = 1; }")
    for offset, key in enumerate(keys[:7]):
        lines.append("    map<%s, Value> map_%s = %d;" % (key, key, numbers[offset]))
    lines.append("    map<string, Kind> kinds = %d;" % numbers[7])
    lines.append("    map<int32, string> strings = %d;" % numbers[8])
    lines.append("    oneof choice")
    lines.append("    {")
    lines.append("        int32 number = %d;" % numbers[9])
    lines.append("        string text = %d;" % numbers[10])
    lines.append("        Value chosen_value = %d;" % numbers[11])
    lines.append("        Kind kind = %d;" % numbers[12])
    lines.append("    }")
    lines.append("    repeated Value values = %d;" % numbers[13])
    lines.append("    optional Value value = %d;" % numbers[14])
    lines.append("    optional Inner inner = %d;" % numbers[15])
    lines.append("    message Inner")
    lines.append("    {")
    lines.append("        map<string, Value> entries = 1;")
    lines.append("        oneof nested_choice { int64 a = 2; bytes b = 3; }")
    lines.append("    }")
    lines.append("}")
    return _case(
        index, "valid", "maps-oneof", "valid-maps-oneof",
        {"case.proto": _source("\n".join(lines) + "\n")}, "case.proto", None,
        "accept", "map key types, message/enum values, oneof and nested map",
        ["syntax.proto2", "package", "message", "map", "map.key.multi",
         "map.value.message", "map.value.enum", "oneof", "oneof.scalar",
         "oneof.message", "oneof.enum", "field.repeated", "message.nested"],
    )


def valid_limits(rng: Rng, index: int) -> Case:
    variant = index % 4
    lines = _header("limits%d" % index)
    if variant == 0:
        lines.append("message Boundary")
        lines.append("{")
        for offset, number in enumerate([1, 15, 16, 2047, 2048, 262143, 262144,
                                         536870911]):
            lines.append("    optional int32 f%d = %d;" % (offset, number))
        lines.append("    reserved 100 to 200, 300, 400 to 500;")
        lines.append("    reserved \"old_name\";")
        lines.append("}")
        detail = "field-number boundaries and reserved ranges"
        coverage = ["field.number.boundary", "reserved"]
    elif variant == 1:
        long_name = "Long" + "x" * 120
        lines.append("message %s { optional int32 %s = 1; }" % (
            long_name, "f" + "y" * 120))
        detail = "long message and field names"
        coverage = ["name.long"]
    elif variant == 2:
        lines.append("message ManyFields")
        lines.append("{")
        for offset in range(40):
            lines.append("    optional int32 f%d = %d;" % (offset, offset + 1))
        lines.append("}")
        detail = "forty fields in one message"
        coverage = ["message.wide"]
    else:
        lines.append("message Level0")
        lines.append("{")
        previous = "Level0"
        for depth in range(1, 7):
            lines.append("    " * depth + "message Level%d" % depth)
            lines.append("    " * depth + "{")
            lines.append("    " * (depth + 1) + "optional Level%d next = 1;"
                         % (depth - 1) if depth > 1 else
                         "    " * (depth + 1) + "optional int32 leaf = 1;")
            lines.append("    " * depth + "}")
        lines.append("    optional Level1 top = 1;")
        lines.append("}")
        detail = "six sibling nested message declarations"
        coverage = ["message.nested"]
    return _case(
        index, "valid", "limits", "valid-limits",
        {"case.proto": _source("\n".join(lines) + "\n")}, "case.proto", None,
        "accept", detail, ["syntax.proto2", "package", "message"] + coverage,
    )


# ---------------------------------------------------------------------------
# Source mutations
# ---------------------------------------------------------------------------


def _mutation_file(body: str, package: str, prologue: str = "",
                   epilogue: str = "") -> str:
    text = prologue
    text += 'syntax = "proto2";\n'
    if package:
        text += "package %s;\n" % package
    text += body
    text += epilogue
    return text


R2_VARIANTS = [
    ("r2-float-octal-08", "reject", "float default 08 is accepted by codegen"),
    ("r2-double-octal-08", "reject", "double default 08 is accepted by codegen"),
    ("r2-float-octal-078", "reject", "float default 078 has no valid octal form"),
    ("r2-string-octal-777", "accept", "octal escape above one byte is refused"),
    ("r2-bytes-octal-400", "accept", "bytes octal escape above one byte is refused"),
    ("r2-string-octal-776", "accept", "octal escape 0x1FE is refused"),
    ("r2-string-surrogate", "accept", "lone UTF-16 surrogate escape is refused"),
    ("r2-string-low-surrogate", "accept", "lone low surrogate escape is refused"),
    ("r2-string-raw-cr", "accept", "raw CR in a string default is refused"),
    ("r2-string-raw-nul", "reject", "raw NUL in a string default"),
    ("r2-string-raw-tab", "accept", "raw tab in a string default"),
    ("r2-nested-comment", "reject", "nested block comment is accepted"),
    ("r2-nul-aggregate", "reject", "NUL inside an aggregate option is accepted"),
    ("r2-int-octal-08", "reject", "int default 08 is invalid octal"),
    ("r2-hex-escape-empty", "reject", "hex escape without digits"),
    ("r2-unknown-escape", "reject", "unknown escape sequence"),
]


def source_mutation_r2(variant: int, rng: Rng, index: int) -> Case:
    name, oracle, detail = R2_VARIANTS[variant % len(R2_VARIANTS)]
    number = rng.pick([1, 2, 7, 100])
    if variant % len(R2_VARIANTS) in (0, 1, 2):
        type_name = "float" if variant % 3 != 1 else "double"
        literal = ["08", "078", "0x1G"][variant % 3]
        body = "message M { optional %s f = %d [default = %s]; }\n" % (
            type_name, number, literal)
        prologue = epilogue = ""
    elif variant % len(R2_VARIANTS) in (3, 4, 5):
        literal = ["\\777", "\\400", "\\776"][variant % 3]
        type_name = "bytes" if variant % 3 == 1 else "string"
        body = 'message M { optional %s s = %d [default = "%s"]; }\n' % (
            type_name, number, literal)
        prologue = epilogue = ""
    elif variant % len(R2_VARIANTS) in (6, 7):
        literal = ["\\ud800", "\\U0000DC00"][variant % 2]
        body = 'message M { optional string s = %d [default = "%s"]; }\n' % (
            number, literal)
        prologue = epilogue = ""
    elif variant % len(R2_VARIANTS) == 8:
        body = 'message M { optional string s = %d [default = "x\ry"]; }\n' % number
        prologue = epilogue = ""
    elif variant % len(R2_VARIANTS) == 9:
        body = 'message M { optional string s = %d [default = "x\x00y"]; }\n' % number
        prologue = epilogue = ""
    elif variant % len(R2_VARIANTS) == 10:
        body = 'message M { optional string s = %d [default = "x\ty"]; }\n' % number
        prologue = epilogue = ""
    elif variant % len(R2_VARIANTS) == 11:
        body = "message M { optional int32 a = 1; }\n"
        prologue = "/* outer /* inner */\n"
        epilogue = ""
    elif variant % len(R2_VARIANTS) == 12:
        body = "message M { optional int32 a = 1; }\n"
        prologue = ""
        epilogue = "option (my.opt) = { a: 1\x00 };\n"
    elif variant % len(R2_VARIANTS) == 13:
        body = "message M { optional int32 a = 1 [default = 08]; }\n"
        prologue = epilogue = ""
    elif variant % len(R2_VARIANTS) == 14:
        body = 'message M { optional string s = 1 [default = "\\x"]; }\n'
        prologue = epilogue = ""
    else:
        body = 'message M { optional string s = 1 [default = "\\q"]; }\n'
        prologue = epilogue = ""
    text = _mutation_file(body, "r2_%d" % index, prologue, epilogue)
    return _case(
        index, "source-mutation", "r2", name,
        {"case.proto": _source(text)}, "case.proto", "R2", oracle, detail,
        ["syntax.proto2", "message", "default.escape", "lexer"],
    )


R5_VARIANTS = [
    ("r5-leading-semicolon", "reject", "leading empty statement before syntax"),
    ("r5-allow-alias-no-alias", "reject", "allow_alias with no shared numbers"),
    ("r5-json-name-enum-value", "reject", "json_name on an enum value"),
    ("r5-plus-option", "reject", "leading plus on a file option value"),
    ("r5-plus-enum-option", "reject", "leading plus on an enum option value"),
    ("r5-oneof-default-int", "accept", "explicit default on a oneof field"),
    ("r5-oneof-default-string", "accept", "string default on a oneof field"),
    ("r5-oneof-default-enum", "accept", "enum default on a oneof field"),
    ("r5-oneof-default-bool", "accept", "bool default on a oneof field"),
    ("r5-leading-semicolon-no-syntax", "accept", "empty statement before package"),
    ("r5-allow-alias-single-value", "reject", "allow_alias with one value"),
    ("r5-json-name-nested-enum", "reject", "json_name on a nested enum value"),
]


def source_mutation_r5(variant: int, rng: Rng, index: int) -> Case:
    v = variant % len(R5_VARIANTS)
    name, oracle, detail = R5_VARIANTS[v]
    package = "r5_%d" % index
    number = rng.pick([1, 2, 5, 50])
    if v == 0:
        text = '; syntax = "proto2";\npackage %s;\nmessage M { optional int32 a = 1; }\n' % package
    elif v in (1, 10):
        text = ('syntax = "proto2";\npackage %s;\n'
                "enum E { option allow_alias = true; A = 0; B = 1; }\n"
                "message M { optional E e = 1 [default = B]; }\n" % package)
    elif v in (2, 11):
        text = ('syntax = "proto2";\npackage %s;\n'
                "message M { enum E { A = 0 [json_name = \"aName\"]; } "
                "optional E e = 1; }\n" % package)
    elif v == 3:
        text = ('syntax = "proto2";\npackage %s;\n'
                "option java_package = +7;\n"
                "message M { optional int32 a = 1; }\n" % package)
    elif v == 4:
        text = ('syntax = "proto2";\npackage %s;\n'
                "option optimize_for = +1;\n"
                "message M { optional int32 a = 1; }\n" % package)
    elif v == 5:
        text = ("syntax = \"proto2\";\npackage %s;\n"
                "message M { oneof o { int32 a = %d [default = 5]; } }\n"
                % (package, number))
    elif v == 6:
        text = ("syntax = \"proto2\";\npackage %s;\n"
                "message M { oneof o { string a = %d [default = \"x\"]; } }\n"
                % (package, number))
    elif v == 7:
        text = ("syntax = \"proto2\";\npackage %s;\n"
                "message M { enum E { A = 0; B = 1; } "
                "oneof o { E e = %d [default = B]; } }\n" % (package, number))
    elif v == 8:
        text = ("syntax = \"proto2\";\npackage %s;\n"
                "message M { oneof o { bool a = %d [default = true]; } }\n"
                % (package, number))
    else:
        text = ('; package %s;\nmessage M { optional int32 a = 1; }\n' % package)
    return _case(
        index, "source-mutation", "r5", name,
        {"case.proto": _source(text)}, "case.proto", "R5", oracle, detail,
        ["syntax.proto2", "message", "grammar"],
    )


E5_VARIANTS = [
    ("e5-keyword-package-class", "package class.foo"),
    ("e5-keyword-package-namespace", "package namespace"),
    ("e5-keyword-package-template", "package template.foo"),
]


def source_mutation_e5(variant: int, rng: Rng, index: int) -> Case:
    name, package = E5_VARIANTS[variant % len(E5_VARIANTS)]
    text = ('syntax = "proto2";\n%s;\n'
            "message M { optional int32 a = 1; }\n" % package)
    return _case(
        index, "source-mutation", "e5", name,
        {"case.proto": _source(text)}, "case.proto", None, "accept",
        "C++ keyword package component %s" % package,
        ["syntax.proto2", "package", "package.keyword"], exception="E5",
    )


# ---------------------------------------------------------------------------
# Descriptor mutations
# ---------------------------------------------------------------------------


def descriptor_mutation(operation: str, base: bytes, index: int) -> Case:
    mutation = mutate.apply_operation(base, operation)
    tag = mutate.OPERATIONS[operation][1]
    exception = tag if tag.startswith("E") else None
    return _case(
        index, "descriptor-mutation", "descriptor", "d-" + operation,
        {"case.pbs": mutation.payload}, mutation.target,
        None if exception else tag, "any", mutation.detail,
        ["descriptor-set", mutation.descriptor_path],
        exception=exception, descriptor_path=mutation.descriptor_path,
    )


# ---------------------------------------------------------------------------
# Plan
# ---------------------------------------------------------------------------

VALID_PLAN = [
    (valid_scalars2, 18),
    (valid_scalars3, 12),
    (valid_defaults, 22),
    (valid_names, 10),
    (valid_policy_names, 4),
    (valid_enums, 10),
    (valid_imports, 16),
    (valid_services, 10),
    (valid_maps_oneof, 12),
    (valid_limits, 6),
]
SOURCE_PLAN = [("r2", 26), ("r5", 33), ("e5", 3)]
DESCRIPTOR_COUNT = 18


def generate_cases(seed: int, count: int, protoc: pathlib.Path,
                   work_dir: pathlib.Path) -> list[Case]:
    """Build ``count`` cases from the fixed 200-case plan.

    The B2a discovery plan is 200 cases: 120 valid trees, 62 source mutations
    and 18 descriptor mutations.  A count above 200 is an integer number of
    consecutive plan rounds (2 000 or 10 000 for the B17 mass runs); the round
    number is mixed into the per-case RNG stream, so the rounds are different
    inputs with the same family balance.  A count of exactly 200 keeps the
    original case-for-case output, which is what ``--check`` compares.
    """
    if count < 200 or count % 200:
        raise ValueError(
            "the discovery plan is 200 cases or a multiple of 200")
    rounds = count // 200
    cases: list[Case] = []
    index = 1
    base = mutate.compile_base(protoc, work_dir)
    for round_index in range(rounds):
        for builder, amount in VALID_PLAN:
            for offset in range(amount):
                rng = Rng((seed * 1_000_003) ^ (index * 7919) ^ 0xC0FFEE
                          ^ (round_index * 0x5DEECE66D))
                case = builder(rng, index)
                case.order = index
                index += 1
                cases.append(case)
        for family, amount in SOURCE_PLAN:
            for offset in range(amount):
                rng = Rng((seed * 2_000_029) ^ (index * 104729) ^ 0xBADF00D
                          ^ (round_index * 0x5DEECE66D))
                variant = round_index * amount + offset
                if family == "r2":
                    case = source_mutation_r2(variant, rng, index)
                elif family == "r5":
                    case = source_mutation_r5(variant, rng, index)
                else:
                    case = source_mutation_e5(variant, rng, index)
                case.order = index
                index += 1
                cases.append(case)
        for offset in range(DESCRIPTOR_COUNT):
            operation_index = round_index * DESCRIPTOR_COUNT + offset
            operation = mutate.OPERATION_ORDER[
                operation_index % len(mutate.OPERATION_ORDER)]
            case = descriptor_mutation(operation, base, index)
            case.order = index
            index += 1
            cases.append(case)
    if len(cases) != count:
        raise RuntimeError("generator produced %d cases, expected %d" % (
            len(cases), count))
    return cases


def write_cases(cases: list[Case], root: pathlib.Path) -> dict:
    cases_dir = root / "cases"
    manifest = {
        "schema": 1,
        "generator": "tests/codegen/harness/generate.py",
        "count": len(cases),
        "cases": [],
    }
    for case in cases:
        directory = cases_dir / case.id
        directory.mkdir(parents=True, exist_ok=True)
        for name, payload in case.files.items():
            (directory / name).write_bytes(payload)
        files = []
        for name in sorted(case.files):
            payload = case.files[name]
            files.append({
                "name": name,
                "size": len(payload),
                "sha256": hashlib.sha256(payload).hexdigest(),
            })
        manifest["cases"].append({
            "id": case.id,
            "order": case.order,
            "mode": case.mode,
            "family": case.family,
            "target": case.target,
            "mechanism": case.mechanism,
            "root": case.root,
            "exception": case.exception,
            "oracle_expect": case.oracle_expect,
            "detail": case.detail,
            "coverage": case.coverage,
            "descriptor_path": case.descriptor_path,
            "files": files,
        })
    (root / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8", newline="\n")
    return manifest


def manifest_digest(manifest: dict) -> str:
    text = json.dumps(manifest, sort_keys=True, ensure_ascii=True)
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--cases", type=int, default=200)
    parser.add_argument("--protoc", type=pathlib.Path, default=DEFAULT_PROTOC)
    parser.add_argument("--out", type=pathlib.Path, default=None)
    parser.add_argument("--check", action="store_true",
                        help="regenerate and compare against the stored manifest")
    args = parser.parse_args()
    args.protoc = runner.resolve_tool(args.protoc)
    root = (args.out or (GEN_ROOT / ("s%d" % args.seed))).resolve()
    cases = generate_cases(args.seed, args.cases, args.protoc, root / "_base")
    if args.check:
        stored = root / "manifest.json"
        if not stored.is_file():
            print("no manifest at %s" % stored, file=sys.stderr)
            return 1
        current = json.loads(stored.read_text(encoding="utf-8"))
        fresh = json.loads(json.dumps({
            "cases": [{
                "id": case.id, "order": case.order, "mode": case.mode,
                "family": case.family, "target": case.target,
                "mechanism": case.mechanism, "root": case.root,
                "exception": case.exception, "oracle_expect": case.oracle_expect,
                "detail": case.detail, "coverage": case.coverage,
                "descriptor_path": case.descriptor_path,
                "files": [{
                    "name": name, "size": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                } for name in sorted(case.files) for payload in [case.files[name]]],
            } for case in cases],
        }))
        if current.get("cases") != fresh["cases"]:
            print("generated cases differ from the stored manifest", file=sys.stderr)
            return 1
        print("generated cases match: %d" % len(cases))
        return 0
    manifest = write_cases(cases, root)
    print("wrote %d cases to %s" % (len(cases), root))
    print("digest: %s" % manifest_digest(manifest))
    counts: dict[str, int] = {}
    for case in cases:
        counts[case.mode] = counts.get(case.mode, 0) + 1
    print("modes: %s" % counts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
