# The parity corpus generator, tracked beside the corpus it writes: Step 11's
# instrument, moved out of the scratch tree so a fresh clone can rebuild the
# table instead of only reading it.
#
# Writes the two-way parity corpus into the repository, measures the protoc
# half of every row, and emits the tsv both readers consume:
#
#   tests/codegen/imports/data/parity/source/<case>.proto  source-origin roots
#   tests/codegen/imports/data/parity/source/<dep>.proto   the shared deps
#   tests/codegen/imports/data/parity/set/<case>.txtpb     descriptor-origin,
#                                                          authored as text
#   tests/codegen/imports/data/parity/set/<case>.pbs       the tracked bytes
#                                                          (A13: protoc's own
#                                                          --encode, then read
#                                                          back with
#                                                          --descriptor_set_in
#                                                          and no --proto_path,
#                                                          from an empty dir)
#   tests/codegen/imports/data/parity/parity-cases.tsv     the row table
#   build/codegen-parity/protoc-verdicts.txt               the measurement log
#                                                          (scratch, gitignored)
#
# Usage:  python tests/codegen/imports/data/parity/generate.py \
#             [--protoc <path>] [--descriptor-proto-path <dir>]
#
# Rebuilding is idempotent by construction: run it on an untouched checkout and
# `git status --short tests/` is empty, which is what makes the tracked corpus
# this generator's output rather than a copy of it.
#
# NAMING RULE. A row is named `<origin>-<slug>[-<discriminator>]-<direction>`,
# where `<origin>` is `s` (source) or `d` (descriptor), `<slug>` is the
# DiagnosticCode enumerator minus its `DIAGNOSTIC_` prefix, lower-cased with
# `_` turned into `-`, and `<direction>` is `accept` or `reject` (or `inverse`
# for a row protoc accepts and the linker refuses by decision). That is what
# makes §11.1's "accept column" a property of the DATA rather than of a
# paragraph: for every row whose `expected` is a code, the row
# `<origin>-<slug>-accept` must exist with `expected = accept`. Both readers
# check that rule against the tsv itself, so a row cannot lose its accept half.
#
# The `linker` field is the row's hypothesis. It is measured by
# `parity_probe`/`test_parity.cpp` before the tsv is frozen; a hypothesis the
# measurement refutes is a fixture or a ledger row, never a silently edited
# expectation.

import argparse
import io
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = HERE
SOURCE = os.path.join(DATA, "source")
SETS = os.path.join(DATA, "set")
TSV = os.path.join(DATA, "parity-cases.tsv")
# <repo>/tests/codegen/imports/data/parity: five levels up to the checkout.
REPO = HERE
for _ in range(5):
    REPO = os.path.dirname(REPO)
LOG = os.path.join(REPO, "build", "codegen-parity", "protoc-verdicts.txt")

PROTOC = os.environ.get("PROTOC") or r"C:\Base\Tools\BIN\protoc.exe"


def find_descriptor_proto(protoc, proto_path=None):
    """(proto path, file argument) for a descriptor.proto protoc can compile.

    A13's encode step needs the `google.protobuf.FileDescriptorSet` definition,
    and the file's own path does not matter to protoc - only that it is
    findable under the proto path. Resolved in this order: the reference source
    tree this project keeps under `build/` when present, the `include/`
    directory a protoc release ships beside its `bin/`, and the copy the
    repository already tracks for the parser differential lane (a flat
    `descriptor.proto`, not `google/protobuf/descriptor.proto`). An explicit
    `proto_path` is used with either spelling inside it, and a relative one
    resolves against the process's current directory - the gate
    (`check_generator.py`) resolves it against the INVOKING directory before
    passing it on, because this generator runs with the corpus as its cwd.
    """
    candidates = [proto_path] if proto_path else [
        os.path.join(REPO, "build", "protobuf-30.1-src", "protobuf-30.1",
                     "src"),
        os.path.join(os.path.dirname(protoc), "..", "include"),
        os.path.join(REPO, "tests", "codegen", "parser", "differential",
                     "corpus"),
    ]
    for candidate in candidates:
        candidate = os.path.abspath(candidate)
        for name in ("google/protobuf/descriptor.proto", "descriptor.proto"):
            if os.path.exists(os.path.join(candidate, name)):
                return candidate, name
    return os.path.abspath(candidates[0]), "google/protobuf/descriptor.proto"


SRC, DESCRIPTOR_PROTO = find_descriptor_proto(PROTOC)

D29_NOTE = ("protoc 30.1 accepts this shape; bind_import_edges refuses it by "
            "A18 (a public/weak index that repeats a dependency is malformed, "
            "and allow_missing relaxes missing targets only) - ledger D-29, "
            "rejected 2026-09-27, pinned by "
            "test_bind_rejects_duplicate_and_overlapping_indexes")
A25_NOTE = ("protoc 30.1 accepts edition = \"2023\"; this project refuses it "
            "by A25 because the model carries no editions semantics - a "
            "recorded capability boundary whose owner is plan 5")
GENERIC_NOTE = ("no counterpart refusal exists: value 0 is not a parity code, "
                "it is the bucket for the loader/CLI classes. The row is the "
                "negative one - a protoc-accepted graph must never come back "
                "DIAGNOSTIC_GENERIC")


def write(rel, text):
    path = os.path.join(DATA, rel)
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)
    return path


def run(args, cwd, stdin_bytes=None):
    done = subprocess.run(args, cwd=cwd, input=stdin_bytes,
                          capture_output=True)
    text = (done.stdout or b"").decode("utf-8", "replace") + \
           (done.stderr or b"").decode("utf-8", "replace")
    return done.returncode, text.strip()


# --------------------------------------------------------------------------
# The shared source dependencies: one name, one text, so no case can pass
# because a sibling happened to redefine it.
# --------------------------------------------------------------------------

SHARED = {
    "dep.proto": 'syntax = "proto3";\npackage s;\n'
                 'message Dep { int32 x = 1; }\n',
    "sub/dep.proto": 'syntax = "proto3";\npackage s;\n'
                     'message SubDep { int32 x = 1; }\n',
    "s-import-cycle-left.proto":
        'syntax = "proto3";\npackage s;\n'
        'import "s-import-cycle-reject.proto";\n'
        'message Left { int32 x = 1; }\n',
    "s-import-cycle-dep.proto": 'syntax = "proto3";\npackage s;\n'
                                'message CycleDep { int32 x = 1; }\n',
}


def source_case(name, root, linker, kind="pair", note=""):
    return dict(name=name, origin="source", files={name + ".proto": root},
                roots=[name + ".proto"], linker=linker, kind=kind, note=note)


A25_EDITION_NOTE = (
    "protoc 30.1 accepts this shape; this project refuses it by A25 because "
    "the model carries no editions semantics - a recorded capability boundary "
    "whose owner is plan 5")


SOURCE_CASES = [
    # --- UNSUPPORTED_SYNTAX: the parser's code channel (S7e, W-60) ---------
    # The source frontend used to answer both shapes with DIAGNOSTIC_GENERIC
    # before any link ran, so the three fixtures carried no row. The parser's
    # throw now carries the descriptor twin's code, so the family is a real
    # source row: a `pair` where protoc refuses the same text, and the
    # recorded `inverse` for editions (protoc accepts it and models
    # `syntax: "editions"`).
    source_case("s-unsupported-syntax-accept",
                'syntax = "proto3";\npackage s;\n'
                'message Root { int32 a = 1; }\n', "accept"),
    source_case("s-unsupported-syntax-reject",
                'syntax = "proto4";\npackage s;\n'
                'message Root { int32 a = 1; }\n',
                "DIAGNOSTIC_UNSUPPORTED_SYNTAX"),
    source_case("s-unsupported-syntax-editions-inverse",
                'edition = "2023";\npackage s;\n'
                'message Root { int32 a = 1; }\n',
                "DIAGNOSTIC_UNSUPPORTED_SYNTAX", kind="inverse",
                note=A25_EDITION_NOTE),

    # --- the proto3 field rules and the enum shapes (S7e, W-60) ------------
    # Each accept twin is the proto2 control of the same declaration, exactly
    # as the descriptor family's accept twins are: a rule that refused the
    # label, the default, the alias or the single value as such - rather than
    # as a shape its syntax forbids - fails this lane instead of passing it.
    source_case("s-proto3-required-field-accept",
                'syntax = "proto2";\npackage s;\n'
                'message Root { required int32 a = 1; }\n', "accept"),
    source_case("s-proto3-required-field-reject",
                'syntax = "proto3";\npackage s;\n'
                'message Root { required int32 a = 1; }\n',
                "DIAGNOSTIC_PROTO3_REQUIRED_FIELD"),
    # D1-A: the source parser synthesizes the same `proto3_optional` flag and
    # synthetic oneof protoc writes, so the accepted row also covers the print
    # and header identity the B12 gate compares.
    source_case("s-proto3-optional-accept",
                'syntax = "proto3";\npackage s;\n'
                'message Root { optional int32 a = 1; }\n', "accept"),
    source_case("s-proto3-explicit-default-accept",
                'syntax = "proto2";\npackage s;\n'
                'message Root { optional int32 a = 1 [default = 5]; }\n',
                "accept"),
    source_case("s-proto3-explicit-default-reject",
                'syntax = "proto3";\npackage s;\n'
                'message Root { int32 a = 1 [default = 5]; }\n',
                "DIAGNOSTIC_PROTO3_EXPLICIT_DEFAULT"),
    source_case("s-duplicate-enum-number-accept",
                'syntax = "proto2";\npackage s;\nenum E {\n'
                '  option allow_alias = true;\n  A = 1;\n  B = 1;\n}\n',
                "accept"),
    source_case("s-duplicate-enum-number-reject",
                'syntax = "proto2";\npackage s;\nenum E { A = 1; B = 1; }\n',
                "DIAGNOSTIC_DUPLICATE_ENUM_NUMBER"),
    source_case("s-empty-enum-accept",
                'syntax = "proto2";\npackage s;\nenum E { A = 1; }\n',
                "accept"),
    source_case("s-empty-enum-reject",
                'syntax = "proto2";\npackage s;\nenum E {}\n',
                "DIAGNOSTIC_EMPTY_ENUM"),

    # --- INVALID_IMPORT: the source spelling rules ------------------------
    source_case("s-invalid-import-accept",
                'syntax = "proto3";\npackage s;\nimport "dep.proto";\n'
                'message Root { Dep d = 1; }\n', "accept"),
    source_case("s-invalid-import-duplicate-reject",
                'syntax = "proto3";\npackage s;\nimport "dep.proto";\n'
                'import "dep.proto";\nmessage Root { int32 a = 1; }\n',
                "DIAGNOSTIC_INVALID_IMPORT"),
    source_case("s-invalid-import-dotdot-reject",
                'syntax = "proto3";\npackage s;\n'
                'import "../dep.proto";\nmessage Root { int32 a = 1; }\n',
                "DIAGNOSTIC_INVALID_IMPORT"),
    source_case("s-invalid-import-backslash-reject",
                'syntax = "proto3";\npackage s;\n'
                'import "sub\\\\dep.proto";\nmessage Root { int32 a = 1; }\n',
                "DIAGNOSTIC_INVALID_IMPORT"),
    source_case("s-invalid-import-absolute-reject",
                'syntax = "proto3";\npackage s;\n'
                'import "/abs/dep.proto";\nmessage Root { int32 a = 1; }\n',
                "DIAGNOSTIC_INVALID_IMPORT"),

    # --- MISSING_IMPORT --------------------------------------------------
    source_case("s-missing-import-accept",
                'syntax = "proto3";\npackage s;\nimport "dep.proto";\n'
                'message Wrap { message Inner { Dep d = 1; } Inner i = 1; }\n',
                "accept"),
    source_case("s-missing-import-reject",
                'syntax = "proto3";\npackage s;\n'
                'import "missing.proto";\nmessage Root { int32 a = 1; }\n',
                "DIAGNOSTIC_MISSING_IMPORT"),

    # --- IMPORT_CYCLE ----------------------------------------------------
    source_case("s-import-cycle-accept",
                'syntax = "proto3";\npackage s;\n'
                'import "s-import-cycle-dep.proto";\n'
                'message Root { CycleDep d = 1; }\n', "accept"),
    source_case("s-import-cycle-reject",
                'syntax = "proto3";\npackage s;\n'
                'import "s-import-cycle-left.proto";\n'
                'message Root { int32 a = 1; }\n',
                "DIAGNOSTIC_IMPORT_CYCLE"),

    # --- Cross-path acceptance (B8, root R6), source origin ----------------
    # CR-21: protoc accepts a method endpoint naming a synthetic map entry, and
    # so does this parser/linker (the ownership rule is a field rule). CR-22: a
    # custom `json_name` shaped like the extension spelling is refused by both;
    # the accept column is the valid custom name beside it.
    source_case(
        "s-map-entry-type-method-accept",
        'syntax = "proto3";\npackage s;\n'
        'message M { map<int32, string> items = 1; }\n'
        'service S { rpc Call (.s.M.ItemsEntry) returns (M); }\n',
        "accept"),
    source_case(
        "s-invalid-json-name-accept",
        'syntax = "proto3";\npackage s;\n'
        'message Root { int32 a = 1 [json_name = "customName"]; }\n',
        "accept"),
    source_case(
        "s-invalid-json-name-reject",
        'syntax = "proto3";\npackage s;\n'
        'message Root { int32 a = 1 [json_name = "[foo]"]; }\n',
        "DIAGNOSTIC_INVALID_JSON_NAME"),
]


# --------------------------------------------------------------------------
# Descriptor-origin cases: the textproto is the fixture, the .pbs is what the
# linker half decodes.
# --------------------------------------------------------------------------

def file_block(name, body, package="p", syntax="proto3"):
    return ('file {\n  name: "' + name + '"\n  package: "' + package + '"\n'
            '  syntax: "' + syntax + '"\n' + body + '}\n')


def root_set(body, package="p"):
    return file_block("root.proto", body, package=package)


DEP = file_block("dep.proto", '  message_type { name: "D" }\n')

USES_DEP = ('  dependency: "dep.proto"\n  message_type {\n'
            '    name: "M"\n'
            '    field { name: "d" number: 1 label: LABEL_OPTIONAL '
            'type: TYPE_MESSAGE type_name: ".p.D" }\n  }\n')

PLAIN_M = ('  message_type {\n    name: "M"\n'
           '    field { name: "a" number: 1 label: LABEL_OPTIONAL '
           'type: TYPE_INT32 }\n  }\n')

PLAIN_S = ('  message_type {\n    name: "S"\n'
           '    field { name: "a" number: 1 label: LABEL_OPTIONAL '
           'type: TYPE_INT32 }\n  }\n')

ENUM_E = ('  enum_type {\n    name: "E"\n'
          '    value { name: "ZERO" number: 0 }\n'
          '    value { name: "ONE" number: 1 }\n  }\n')


def map_set(value_field, key_type="TYPE_STRING"):
    return root_set(
        '  message_type {\n    name: "MapHolder"\n'
        '    nested_type {\n      name: "ItemsEntry"\n'
        '      options { map_entry: true }\n'
        '      field { name: "key" number: 1 label: LABEL_OPTIONAL '
        'type: ' + key_type + ' }\n' + value_field +
        '    }\n'
        '    field { name: "items" number: 1 label: LABEL_REPEATED '
        'type: TYPE_MESSAGE type_name: ".p.MapHolder.ItemsEntry" }\n  }\n')


ENTRY_VALUE_INT32 = ('      field { name: "value" number: 2 label: '
                     'LABEL_OPTIONAL type: TYPE_INT32 }\n')


def invisible_set(public):
    """root imports mid; mid imports dep, publicly only in the accept twin."""
    mid = file_block("mid.proto",
                     '  dependency: "dep.proto"\n' +
                     ('  public_dependency: 0\n' if public else ""))
    return (DEP + mid +
            root_set('  dependency: "mid.proto"\n  message_type {\n'
                     '    name: "M"\n'
                     '    field { name: "d" number: 1 label: '
                     'LABEL_OPTIONAL type: TYPE_MESSAGE '
                     'type_name: ".p.D" }\n  }\n'))


def descriptor_case(name, text, linker, kind="pair", note=""):
    return dict(name=name, origin="descriptor", text=text, linker=linker,
                kind=kind, note=note)


# --------------------------------------------------------------------------
# Post-Step-11 measured shapes (fix rounds r25, r26, r28), added through this
# builder so the corpus stays its own single source of truth.
#
# Provenance: build/plan4/fix-r25/probe/parity.txt (D-51, D-52..D-57),
# build/plan4/fix-r26/own/results-after.txt (D-61, D-62) and
# build/plan4/fix-r28/own/after-run.txt (the nested map-entry collision;
# the round's own PROTOC line is a `--decode`, which answers 0 for any
# decodable set, so the build verdict is re-measured here instead of taken
# from it).
# Each shape was measured by its round against libprotoc 30.1 through A13;
# the `linker` cell is the hypothesis the C++ half re-measures.
#
# Shapes whose refusal carries DIAGNOSTIC_GENERIC in this project cannot be
# rows (section 11.6 forbids naming the bucket) and are kept as probe cases
# below: [packed = true] on a non-packable field, a proto3 group, a repeated
# field's default and a oneof member's label. The same section carries the one
# measured shape whose disagreement has no `kind` at all - the plain-message
# extension member (protoc refuses it, this linker links it, clause 12's
# boundary).
# --------------------------------------------------------------------------

A26_UNREFERENCED_NOTE = (
    "protoc 30.1 accepts this shape; A26's sweep walks every nested map entry "
    "whether or not a field references it (the reference-free strictness, "
    "measured 2026-09-29 by fix round r25 and re-derived by fix round r26)")

INVALID_FIELD_TYPE_NOTE = (
    "protoc 30.1 accepts this shape and resolves it: an absent kind is the "
    "proto default (TYPE_DOUBLE), an unknown kind the same way, a group a "
    "generated group class. This project refuses all three by the owner's "
    "decision of 2026-10-21 (plan 5 follow-up, session S1, W-10): the "
    "generator has no group types at all, and the emitter used to write "
    "\"???type\" or \"???group\" into the header at rc=0 - a deliberate "
    "\"stricter than protoc\" divergence, like the package-depth and the "
    "output-root rules")

ENTRY_KEY_INT32 = ('      field { name: "key" number: 1 label: '
                   'LABEL_OPTIONAL type: TYPE_INT32 }\n')
ENTRY_KEY_STRING = ('      field { name: "key" number: 1 label: '
                    'LABEL_OPTIONAL type: TYPE_STRING }\n')
REQ_M = PLAIN_M.replace("LABEL_OPTIONAL", "LABEL_REQUIRED")
EXTENSION_RANGE = '      extension_range { start: 10 end: 20 }\n'
EXTENSION_MEMBER = ('      extension { name: "e" number: 100 label: '
                    'LABEL_OPTIONAL type: TYPE_INT32 extendee: ".p.MapHolder" '
                    '}\n')


def no_syntax_set(body, package="p"):
    """A file with no `syntax` field: protoc's own default is proto2, which is
    the shape fix round r26 crafted for the extension cases."""
    return ('file {\n  name: "root.proto"\n  package: "' + package + '"\n' +
            body + '}\n')


def entry_shape(entry_body, reference=True, owner="MapHolder", syntax="proto3",
                prefix=""):
    """A holder message with an ItemsEntry carrying `entry_body`.

    `reference=False` leaves the entry unreferenced - the A26 shape r25 and r26
    both measured (protoc accepts it, this linker does not). `syntax=None`
    omits the syntax field, protoc's proto2 default."""
    body = (prefix + '  message_type {\n    name: "' + owner + '"\n'
            '    nested_type {\n      name: "ItemsEntry"\n'
            '      options { map_entry: true }\n' + entry_body +
            '    }\n')
    if reference:
        body += ('    field { name: "items" number: 1 label: LABEL_REPEATED '
                 'type: TYPE_MESSAGE type_name: ".p.' + owner +
                 '.ItemsEntry" }\n')
    body += '  }\n'
    if syntax is None:
        return no_syntax_set(body)
    return root_set(body, package="p")


def edition_set(body, edition, syntax=None):
    """A file whose classification is decided by `edition` alone (no syntax
    string) or by a syntax string beside it."""
    head = 'file {\n  name: "root.proto"\n  package: "p"\n'
    if syntax is not None:
        head += '  syntax: "' + syntax + '"\n'
    if edition is not None:
        head += '  edition: ' + edition + '\n'
    return head + body + '}\n'


def method_block(name):
    return ('    method { name: "' + name + '" input_type: ".p.Req" '
            'output_type: ".p.Resp" }\n')


def service_set(methods):
    return file_block("root.proto",
                      '  message_type { name: "Req" }\n'
                      '  message_type { name: "Resp" }\n'
                      '  service {\n    name: "S"\n' + methods + '  }\n',
                      syntax="proto2")


def duplicate_entries_set(second_plain=False):
    """Fix round r28's collision shapes: two same-named nested declarations in
    one Holder - both legal map entries, or the second one a plain message."""
    def entry(key_type, value_type):
        return ('      nested_type {\n        name: "ItemsEntry"\n'
                '        options { map_entry: true }\n'
                '        field { name: "key" number: 1 label: LABEL_OPTIONAL '
                'type: ' + key_type + ' }\n'
                '        field { name: "value" number: 2 label: LABEL_OPTIONAL '
                'type: ' + value_type + ' }\n      }\n')
    second = ('      nested_type { name: "ItemsEntry" }\n' if second_plain
              else entry("TYPE_INT64", "TYPE_STRING"))
    return root_set(
        '  message_type {\n    name: "Holder"\n' +
        entry("TYPE_STRING", "TYPE_INT32") + second +
        '    field { name: "items" number: 1 label: LABEL_REPEATED '
        'type: TYPE_MESSAGE type_name: ".p.Holder.ItemsEntry" }\n  }\n')


def descriptor_probe_case(name, text, note=""):
    """A measured shape kept for the record with NO row: either the verdict
    cannot name a parity code (section 11.6) or the disagreement has no kind.
    It is measured by every builder run and lands in the verdict log beside
    the rows."""
    return dict(name=name, origin="descriptor", text=text, linker="accept",
                kind="pair", note=note, no_row=True)


BS = chr(92)


EXTRA_CASES = [
    # --- MALFORMED_MAP_ENTRY: the clause shapes of the A26 sweep -----------
    # Fix round r25 (D-53) measured these shapes through --descriptor_set_in
    # and pinned the refusals with test_malformed_map_entry_shape_is_rejected;
    # fix round r26 (D-62) added the extension data and fix round r27
    # re-derived them with its own encoder. Each is a parity pair, and the
    # code's accept twin is the existing d-malformed-map-entry-accept, so a
    # sweep that refused every entry would fail this lane.
    descriptor_case(
        "d-malformed-map-entry-bytes-key-reject",
        entry_shape(ENTRY_KEY_INT32.replace("TYPE_INT32", "TYPE_BYTES") +
                    ENTRY_VALUE_INT32),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-float-key-reject",
        entry_shape(ENTRY_KEY_INT32.replace("TYPE_INT32", "TYPE_FLOAT") +
                    ENTRY_VALUE_INT32),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-enum-key-reject",
        entry_shape(ENTRY_KEY_INT32.replace(
                        "TYPE_INT32", 'TYPE_ENUM type_name: ".p.E"') +
                    ENTRY_VALUE_INT32, prefix=ENUM_E),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-third-field-reject",
        entry_shape(ENTRY_KEY_INT32 + ENTRY_VALUE_INT32 +
                    '      field { name: "extra" number: 3 label: '
                    'LABEL_OPTIONAL type: TYPE_INT32 }\n'),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-key-renamed-reject",
        entry_shape(ENTRY_KEY_INT32.replace('"key"', '"k"') +
                    ENTRY_VALUE_INT32),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-value-renamed-reject",
        entry_shape(ENTRY_KEY_INT32 +
                    ENTRY_VALUE_INT32.replace('"value"', '"v"')),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-key-repeated-reject",
        entry_shape(ENTRY_KEY_INT32.replace("LABEL_OPTIONAL",
                                            "LABEL_REPEATED") +
                    ENTRY_VALUE_INT32),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-nested-type-reject",
        entry_shape(ENTRY_KEY_INT32 + ENTRY_VALUE_INT32 +
                    '      nested_type { name: "N" }\n'),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-nested-enum-reject",
        entry_shape(ENTRY_KEY_INT32 + ENTRY_VALUE_INT32 +
                    '      enum_type { name: "E" value { name: "ZERO" '
                    'number: 0 } }\n'),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-extension-range-reject",
        entry_shape(ENTRY_KEY_INT32 + ENTRY_VALUE_INT32 + EXTENSION_RANGE,
                    syntax=None),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-extension-member-reject",
        entry_shape(ENTRY_KEY_INT32 + ENTRY_VALUE_INT32 + EXTENSION_MEMBER,
                    syntax=None),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-extension-both-reject",
        entry_shape(ENTRY_KEY_INT32 + ENTRY_VALUE_INT32 + EXTENSION_RANGE +
                    EXTENSION_MEMBER, syntax=None),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-extension-range-string-key-reject",
        entry_shape(ENTRY_KEY_STRING + ENTRY_VALUE_INT32 + EXTENSION_RANGE,
                    syntax=None),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-extension-range-missing-value-reject",
        entry_shape(ENTRY_KEY_INT32 +
                    ENTRY_VALUE_INT32.replace("number: 2", "number: 3") +
                    EXTENSION_RANGE, syntax=None),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),
    descriptor_case(
        "d-malformed-map-entry-bytes-key-unreferenced-inverse",
        file_block("root.proto",
                   '  message_type {\n    name: "MEntry"\n'
                   '    options { map_entry: true }\n'
                   '    field { name: "key" number: 1 label: LABEL_OPTIONAL '
                   'type: TYPE_BYTES }\n'
                   '    field { name: "value" number: 2 label: LABEL_OPTIONAL '
                   'type: TYPE_INT32 }\n  }\n'),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY", kind="inverse",
        note=A26_UNREFERENCED_NOTE),
    descriptor_case(
        "d-malformed-map-entry-extension-range-unreferenced-inverse",
        entry_shape(ENTRY_KEY_INT32 + ENTRY_VALUE_INT32 + EXTENSION_RANGE,
                    reference=False, syntax=None),
        "DIAGNOSTIC_MALFORMED_MAP_ENTRY", kind="inverse",
        note=A26_UNREFERENCED_NOTE),
    descriptor_case(
        "d-malformed-map-entry-plain-extension-range-accept",
        no_syntax_set('  message_type {\n    name: "Plain"\n'
                      '    field { name: "a" number: 1 label: LABEL_OPTIONAL '
                      'type: TYPE_INT32 }\n' + EXTENSION_RANGE + '  }\n'),
        "accept"),
    descriptor_case(
        "d-malformed-map-entry-nested-plain-extension-range-accept",
        no_syntax_set('  message_type {\n    name: "Outer"\n'
                      '    field { name: "a" number: 1 label: LABEL_OPTIONAL '
                      'type: TYPE_INT32 }\n'
                      '    nested_type {\n      name: "Inner"\n'
                      '      field { name: "b" number: 1 label: '
                      'LABEL_OPTIONAL type: TYPE_INT32 }\n' +
                      EXTENSION_RANGE + '    }\n  }\n'),
        "accept"),

    # --- INVALID_SYMBOL_NAME / DUPLICATE_SYMBOL: method names (r25, D-57) --
    descriptor_case("d-invalid-symbol-name-method-reject",
                    service_set(method_block("bad$name")),
                    "DIAGNOSTIC_INVALID_SYMBOL_NAME"),
    descriptor_case("d-duplicate-symbol-method-reject",
                    service_set(method_block("go") + method_block("go")),
                    "DIAGNOSTIC_DUPLICATE_SYMBOL"),
    descriptor_case("d-invalid-symbol-name-method-accept",
                    service_set(method_block("go")), "accept"),

    # --- The D-51 accept controls: packed=false anywhere is legal ----------
    descriptor_case(
        "d-packed-false-optional-accept",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "a" number: 1 label: LABEL_OPTIONAL '
                   'type: TYPE_INT32 options { packed: false } }\n  }\n',
                   syntax="proto2"),
        "accept"),
    descriptor_case(
        "d-packed-false-string-accept",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "s" number: 1 label: LABEL_REPEATED '
                   'type: TYPE_STRING options { packed: false } }\n  }\n',
                   syntax="proto2"),
        "accept"),
    descriptor_case(
        "d-packed-false-message-accept",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "n" number: 1 label: LABEL_REPEATED '
                   'type: TYPE_MESSAGE type_name: ".p.M.N" '
                   'options { packed: false } }\n'
                   '    nested_type { name: "N" }\n  }\n',
                   syntax="proto2"),
        "accept"),

    # --- The group and oneof controls (r25, D-54/D-56) ---------------------
    # A proto2 group is one of the three shapes the plan-5 follow-up's S1
    # (W-10) refuses: the generator has no group types, and the emitter used
    # to write "???group" for one. protoc accepts the shape and generates a
    # group class for it, so the row is an INVERSE one - the disagreement is
    # by decision, and the decision is recorded in the ledger's Decisions
    # table. The accept column of the code it answers with is
    # d-invalid-field-type-accept, below.
    descriptor_case(
        "d-invalid-field-type-group-reject",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "g" number: 1 label: LABEL_OPTIONAL '
                   'type: TYPE_GROUP type_name: ".p.M.G" }\n'
                   '    nested_type { name: "G" }\n  }\n',
                   syntax="proto2"),
        "DIAGNOSTIC_INVALID_FIELD_TYPE", kind="inverse",
        note=INVALID_FIELD_TYPE_NOTE),
    descriptor_case(
        "d-oneof-member-optional-accept",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    oneof_decl { name: "pick" }\n'
                   '    field { name: "a" number: 1 label: LABEL_OPTIONAL '
                   'type: TYPE_INT32 oneof_index: 0 }\n  }\n',
                   syntax="proto2"),
        "accept"),

    # --- Editions (r25 D-52, r26 D-61): A25's class, shape by shape --------
    descriptor_case("d-unsupported-syntax-editions-only-reject",
                    file_block("root.proto", PLAIN_M, syntax="editions"),
                    "DIAGNOSTIC_UNSUPPORTED_SYNTAX"),
    descriptor_case("d-unsupported-syntax-edition-2024-reject",
                    edition_set(PLAIN_M, "EDITION_2024"),
                    "DIAGNOSTIC_UNSUPPORTED_SYNTAX"),
    descriptor_case("d-unsupported-syntax-edition-2023-only-inverse",
                    edition_set(PLAIN_M, "EDITION_2023"),
                    "DIAGNOSTIC_UNSUPPORTED_SYNTAX", kind="inverse",
                    note=A25_EDITION_NOTE),
    descriptor_case("d-unsupported-syntax-edition-2023-proto3-inverse",
                    edition_set(PLAIN_M, "EDITION_2023", syntax="proto3"),
                    "DIAGNOSTIC_UNSUPPORTED_SYNTAX", kind="inverse",
                    note=A25_EDITION_NOTE),
    descriptor_case("d-unsupported-syntax-empty-accept",
                    file_block("root.proto", PLAIN_M, syntax=""),
                    "accept"),
    descriptor_case("d-unsupported-syntax-edition-proto2-accept",
                    edition_set(REQ_M, "EDITION_PROTO2"), "accept"),
    descriptor_case(
        "d-proto3-required-field-edition-proto3-reject",
        edition_set(REQ_M, "EDITION_PROTO3"),
        "DIAGNOSTIC_PROTO3_REQUIRED_FIELD"),
    descriptor_case(
        "d-proto3-required-field-proto3-edition-proto2-accept",
        edition_set(REQ_M, "EDITION_PROTO2", syntax="proto3"), "accept"),

    # --- The nested map-entry name collision (r28) -------------------------
    # Fix round r28 measured these shapes and changed the linker to refuse
    # the declaration by name collision in both modes (pinned by
    # test_duplicate_map_entry_name_is_a_name_collision). The build verdict
    # re-measured 2026-10-02 is REJECT for both, so each row is a parity
    # pair, not an inverse one.
    descriptor_case(
        "d-duplicate-symbol-entries-collision-reject",
        duplicate_entries_set(),
        "DIAGNOSTIC_DUPLICATE_SYMBOL"),
    descriptor_case(
        "d-duplicate-symbol-entry-and-plain-reject",
        duplicate_entries_set(second_plain=True),
        "DIAGNOSTIC_DUPLICATE_SYMBOL"),
    descriptor_case(
        "d-duplicate-symbol-distinct-entries-accept",
        root_set(
            '  message_type {\n    name: "Holder"\n'
            '    nested_type {\n      name: "ItemsEntry"\n'
            '      options { map_entry: true }\n'
            '      field { name: "key" number: 1 label: LABEL_OPTIONAL '
            'type: TYPE_STRING }\n'
            '      field { name: "value" number: 2 label: LABEL_OPTIONAL '
            'type: TYPE_INT32 }\n    }\n'
            '    nested_type {\n      name: "OthersEntry"\n'
            '      options { map_entry: true }\n'
            '      field { name: "key" number: 1 label: LABEL_OPTIONAL '
            'type: TYPE_INT64 }\n'
            '      field { name: "value" number: 2 label: LABEL_OPTIONAL '
            'type: TYPE_STRING }\n    }\n'
            '    field { name: "items" number: 1 label: LABEL_REPEATED '
            'type: TYPE_MESSAGE type_name: ".p.Holder.ItemsEntry" }\n'
            '    field { name: "others" number: 2 label: LABEL_REPEATED '
            'type: TYPE_MESSAGE type_name: ".p.Holder.OthersEntry" }\n  }\n'),
        "accept"),
    descriptor_case(
        "d-duplicate-symbol-scoped-entries-accept",
        root_set(
            '  message_type {\n    name: "Holder"\n'
            '    nested_type {\n      name: "ItemsEntry"\n'
            '      options { map_entry: true }\n'
            '      field { name: "key" number: 1 label: LABEL_OPTIONAL '
            'type: TYPE_STRING }\n'
            '      field { name: "value" number: 2 label: LABEL_OPTIONAL '
            'type: TYPE_INT32 }\n    }\n'
            '    field { name: "items" number: 1 label: LABEL_REPEATED '
            'type: TYPE_MESSAGE type_name: ".p.Holder.ItemsEntry" }\n  }\n'
            '  message_type {\n    name: "Other"\n'
            '    nested_type {\n      name: "ItemsEntry"\n'
            '      options { map_entry: true }\n'
            '      field { name: "key" number: 1 label: LABEL_OPTIONAL '
            'type: TYPE_INT64 }\n'
            '      field { name: "value" number: 2 label: LABEL_OPTIONAL '
            'type: TYPE_STRING }\n    }\n'
            '    field { name: "items" number: 1 label: LABEL_REPEATED '
            'type: TYPE_MESSAGE type_name: ".p.Other.ItemsEntry" }\n  }\n'),
        "accept"),

    # --- Cross-path acceptance (B8, root R6) -------------------------------
    # CR-21: a method endpoint naming a synthetic map entry. protoc's
    # ownership rule is a FIELD rule (`ValidateMapEntry` is reached only from
    # `ValidateFieldOptions`), so a method endpoint names an ordinary message
    # type and the reference compiler accepts the shape on both paths. The
    # field control beside it (`d-map-entry-type-reject`) stays refused by
    # both, which is what keeps the rule narrow.
    descriptor_case(
        "d-map-entry-type-method-accept",
        root_set(
            '  message_type {\n    name: "MapHolder"\n'
            '    nested_type {\n      name: "ItemsEntry"\n'
            '      options { map_entry: true }\n'
            '      field { name: "key" number: 1 label: LABEL_OPTIONAL '
            'type: TYPE_INT32 }\n'
            '      field { name: "value" number: 2 label: LABEL_OPTIONAL '
            'type: TYPE_STRING }\n    }\n'
            '    field { name: "items" number: 1 label: LABEL_REPEATED '
            'type: TYPE_MESSAGE type_name: ".p.MapHolder.ItemsEntry" }\n  }\n'
            '  service {\n    name: "S"\n'
            '    method { name: "Call" input_type: '
            '".p.MapHolder.ItemsEntry" output_type: ".p.MapHolder" }\n  }\n'),
        "accept"),

    # CR-22: a custom `json_name` shaped like the extension spelling. protoc
    # refuses it in both syntaxes (`JsonNameLooksLikeExtension`,
    # descriptor.cc:6519-6531), and the accept column is the valid custom name
    # beside it - the rule must not refuse every custom name.
    descriptor_case(
        "d-invalid-json-name-accept",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "a" number: 1 label: LABEL_OPTIONAL '
                   'type: TYPE_INT32 json_name: "customName" }\n  }\n'),
        "accept"),
    descriptor_case(
        "d-invalid-json-name-reject",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "a" number: 1 label: LABEL_OPTIONAL '
                   'type: TYPE_INT32 json_name: "[foo]" }\n  }\n'),
        "DIAGNOSTIC_INVALID_JSON_NAME"),

    # --- Measured shapes with NO row (their verdict cannot be a row) ------
    descriptor_probe_case(
        "probe-descriptor-packed-true",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "s" number: 1 label: LABEL_REPEATED '
                   'type: TYPE_STRING options { packed: true } }\n  }\n',
                   syntax="proto2"),
        note="refused here with DIAGNOSTIC_GENERIC (kind-independent "
             "validation), which no row may name; protoc refuses it too - "
             "fix round r25, D-51"),
    descriptor_probe_case(
        "probe-descriptor-proto3-group",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "g" number: 1 label: LABEL_OPTIONAL '
                   'type: TYPE_GROUP type_name: ".p.M.G" }\n'
                   '    nested_type { name: "G" }\n  }\n'),
        note="refused here with DIAGNOSTIC_GENERIC; protoc refuses it too - "
             "fix round r25, D-54"),
    descriptor_probe_case(
        "probe-descriptor-repeated-default",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "a" number: 1 label: LABEL_REPEATED '
                   'type: TYPE_INT32 default_value: "5" }\n  }\n',
                   syntax="proto2"),
        note="refused here with DIAGNOSTIC_GENERIC; protoc refuses it too - "
             "fix round r25, D-55"),
    descriptor_probe_case(
        "probe-descriptor-oneof-repeated",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    oneof_decl { name: "pick" }\n'
                   '    field { name: "a" number: 1 label: LABEL_REPEATED '
                   'type: TYPE_INT32 oneof_index: 0 }\n  }\n',
                   syntax="proto2"),
        note="refused here with DIAGNOSTIC_GENERIC; protoc refuses it too - "
             "fix round r25, D-56"),
    descriptor_probe_case(
        "probe-descriptor-oneof-required",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    oneof_decl { name: "pick" }\n'
                   '    field { name: "a" number: 1 label: LABEL_REQUIRED '
                   'type: TYPE_INT32 oneof_index: 0 }\n  }\n',
                   syntax="proto2"),
        note="refused here with DIAGNOSTIC_GENERIC; protoc refuses it too - "
             "fix round r25, D-56"),
    descriptor_probe_case(
        "probe-descriptor-plain-extension-member",
        no_syntax_set('  message_type {\n    name: "Plain"\n'
                      '    field { name: "a" number: 1 label: LABEL_OPTIONAL '
                      'type: TYPE_INT32 }\n'
                      '    extension { name: "e" number: 100 label: '
                      'LABEL_OPTIONAL type: TYPE_INT32 extendee: ".p.Plain" }'
                      '\n  }\n'),
        note="protoc REFUSES this shape (the extendee declares no such "
             "extension number) and this linker LINKS it: clause 12 keeps "
             "extension metadata out of the model, so the disagreement has no "
             "kind and cannot be a row - fix round r26, recorded divergence"),
    descriptor_probe_case(
        "probe-descriptor-bytes-default-unknown-escape",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "b" number: 1 label: LABEL_OPTIONAL '
                   'type: TYPE_BYTES default_value: "' + BS + BS + 'q" }\n'
                   '  }\n',
                   syntax="proto2"),
        note="protoc REFUSES this shape (Invalid escaping in default "
             "value.) and this linker refuses it too, with "
             "DIAGNOSTIC_GENERIC - the C-escape grammar check fix round "
             "r25 added (D-58), pinned by "
             "test_descriptor_origin_bytes_default_escape_is_validated; a "
             "refusal that answers with the bucket cannot be a row"),
    descriptor_probe_case(
        "probe-descriptor-bytes-default-trailing-backslash",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "b" number: 1 label: LABEL_OPTIONAL '
                   'type: TYPE_BYTES default_value: "abc' + BS + BS + '" }\n'
                   '  }\n',
                   syntax="proto2"),
        note="protoc REFUSES this shape (a trailing backslash is not a "
             "complete escape); this linker refuses it with "
             "DIAGNOSTIC_GENERIC (D-58)"),
    descriptor_probe_case(
        "probe-descriptor-bytes-default-octal-overflow",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "b" number: 1 label: LABEL_OPTIONAL '
                   'type: TYPE_BYTES default_value: "' + BS + BS + '400" }\n'
                   '  }\n',
                   syntax="proto2"),
        note="protoc REFUSES this shape (one to three octal digits at "
             "most 255); this linker refuses it with DIAGNOSTIC_GENERIC "
             "(D-58)"),
    descriptor_probe_case(
        "probe-descriptor-bytes-default-string-twin",
        file_block("root.proto",
                   '  message_type {\n    name: "M"\n'
                   '    field { name: "b" number: 1 label: LABEL_OPTIONAL '
                   'type: TYPE_STRING default_value: "' + BS + BS + 'q" }\n'
                   '  }\n',
                   syntax="proto2"),
        note="the control: a STRING default is not C-unescaped on either "
             "side, so the same bytes are accepted by protoc and by this "
             "linker (D-58, its own accept control)"),
]

DESCRIPTOR_CASES = [
    # --- DIAGNOSTIC_GENERIC: the negative row -----------------------------
    descriptor_case(
        "d-generic-not-raised",
        DEP + file_block("mid.proto", '  dependency: "dep.proto"\n') +
        root_set('  dependency: "dep.proto"\n  dependency: "mid.proto"\n'
                 '  enum_type {\n    name: "E"\n'
                 '    value { name: "ZERO" number: 0 }\n'
                 '    value { name: "ONE" number: 1 }\n  }\n'
                 '  message_type {\n    name: "M"\n'
                 '    oneof_decl { name: "pick" }\n'
                 '    field { name: "d" number: 1 label: LABEL_OPTIONAL '
                 'type: TYPE_MESSAGE type_name: ".p.D" }\n'
                 '    field { name: "e" number: 2 label: LABEL_OPTIONAL '
                 'type: TYPE_ENUM type_name: ".p.E" oneof_index: 0 }\n  }\n'),
        "accept", kind="none", note=GENERIC_NOTE),

    # --- UNSUPPORTED_SYNTAX: the capability statement A25 owns ------------
    descriptor_case("d-unsupported-syntax-accept", root_set(PLAIN_M),
                    "accept"),
    descriptor_case("d-unsupported-syntax-reject",
                    file_block("root.proto", "", syntax="proto4"),
                    "DIAGNOSTIC_UNSUPPORTED_SYNTAX"),
    descriptor_case(
        "d-unsupported-syntax-editions-inverse",
        # protoc 30.1's own encoding of `edition = "2023";` carries both
        # fields, and classify_file_syntax is keyed on the syntax string, so
        # this is the shape the refusal is about.
        root_set(PLAIN_M).replace('syntax: "proto3"',
                                  'syntax: "editions"\n  '
                                  'edition: EDITION_2023'),
        "DIAGNOSTIC_UNSUPPORTED_SYNTAX", kind="inverse", note=A25_NOTE),

    # --- UNRESOLVED_TYPE --------------------------------------------------
    descriptor_case("d-unresolved-type-accept", DEP + root_set(USES_DEP),
                    "accept"),
    descriptor_case("d-unresolved-type-reject",
                    DEP + root_set(
                        '  dependency: "dep.proto"\n  message_type {\n'
                        '    name: "M"\n'
                        '    field { name: "m" number: 1 label: '
                        'LABEL_OPTIONAL type: TYPE_MESSAGE '
                        'type_name: ".p.Missing" }\n  }\n'),
                    "DIAGNOSTIC_UNRESOLVED_TYPE"),

    # --- INVISIBLE_TYPE ---------------------------------------------------
    descriptor_case("d-invisible-type-accept", invisible_set(True), "accept"),
    descriptor_case("d-invisible-type-reject", invisible_set(False),
                    "DIAGNOSTIC_INVISIBLE_TYPE"),

    # --- DUPLICATE_SYMBOL -------------------------------------------------
    descriptor_case("d-duplicate-symbol-accept",
                    root_set('  message_type { name: "M" }\n'
                             '  message_type { name: "N" }\n'), "accept"),
    descriptor_case("d-duplicate-symbol-reject",
                    root_set('  message_type { name: "M" }\n'
                             '  message_type { name: "M" }\n'),
                    "DIAGNOSTIC_DUPLICATE_SYMBOL"),

    # --- NOT_A_TYPE -------------------------------------------------------
    descriptor_case("d-not-a-type-accept",
                    root_set('  service { name: "S" }\n  message_type {\n'
                             '    name: "M"\n'
                             '    field { name: "m" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_MESSAGE '
                             'type_name: ".p.M" }\n  }\n'), "accept"),
    descriptor_case("d-not-a-type-reject",
                    root_set('  service { name: "S" }\n  message_type {\n'
                             '    name: "M"\n'
                             '    field { name: "s" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_MESSAGE '
                             'type_name: ".p.S" }\n  }\n'),
                    "DIAGNOSTIC_NOT_A_TYPE"),

    # --- MAP_ENTRY_TYPE: an entry used as an ordinary field ---------------
    descriptor_case(
        "d-map-entry-type-accept",
        map_set('      field { name: "value" number: 2 label: LABEL_OPTIONAL '
                'type: TYPE_STRING }\n', "TYPE_INT32"), "accept"),
    descriptor_case(
        "d-map-entry-type-reject",
        root_set(
            '  message_type {\n    name: "Holder"\n'
            '    nested_type {\n      name: "ItemsEntry"\n'
            '      options { map_entry: true }\n'
            '      field { name: "key" number: 1 label: LABEL_OPTIONAL '
            'type: TYPE_STRING }\n' + ENTRY_VALUE_INT32 +
            '    }\n'
            '    field { name: "one" number: 1 label: LABEL_OPTIONAL '
            'type: TYPE_MESSAGE type_name: ".p.Holder.ItemsEntry" }\n  }\n'),
        "DIAGNOSTIC_MAP_ENTRY_TYPE"),

    # --- MALFORMED_MAP_ENTRY ---------------------------------------------
    descriptor_case("d-malformed-map-entry-accept",
                    map_set(ENTRY_VALUE_INT32), "accept"),
    descriptor_case("d-malformed-map-entry-reject",
                    map_set(""),
                    "DIAGNOSTIC_MALFORMED_MAP_ENTRY"),

    # --- KIND_MISMATCH ----------------------------------------------------
    descriptor_case("d-kind-mismatch-accept",
                    root_set(ENUM_E +
                             '  message_type {\n    name: "M"\n'
                             '    field { name: "e" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_ENUM '
                             'type_name: ".p.E" }\n  }\n'), "accept"),
    descriptor_case("d-kind-mismatch-reject",
                    root_set(ENUM_E +
                             '  message_type {\n    name: "M"\n'
                             '    field { name: "e" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_MESSAGE '
                             'type_name: ".p.E" }\n  }\n'),
                    "DIAGNOSTIC_KIND_MISMATCH"),

    # --- INVALID_FIELD_NUMBER ---------------------------------------------
    descriptor_case("d-invalid-field-number-accept",
                    root_set('  message_type {\n    name: "M"\n'
                             '    field { name: "a" number: 20000 label: '
                             'LABEL_OPTIONAL type: TYPE_INT32 }\n  }\n'),
                    "accept"),
    descriptor_case("d-invalid-field-number-zero-reject",
                    root_set('  message_type {\n    name: "M"\n'
                             '    field { name: "a" number: 0 label: '
                             'LABEL_OPTIONAL type: TYPE_INT32 }\n  }\n'),
                    "DIAGNOSTIC_INVALID_FIELD_NUMBER"),
    descriptor_case("d-invalid-field-number-reserved-reject",
                    root_set('  message_type {\n    name: "M"\n'
                             '    field { name: "a" number: 19000 label: '
                             'LABEL_OPTIONAL type: TYPE_INT32 }\n  }\n'),
                    "DIAGNOSTIC_INVALID_FIELD_NUMBER"),

    # --- MISSING_TYPE_NAME, and the P-19 control --------------------------
    # The accept column is a field that names no type: a SCALAR needs no
    # type_name, so the field is legal and the rule does not reach it. It
    # used to carry no kind at all, which the S1 refusal (W-10) turned into
    # a refusal of its own - the accept column of this code needs a shape the
    # code still accepts.
    descriptor_case("d-missing-type-name-accept",
                    root_set('  message_type {\n    name: "M"\n'
                             '    field { name: "a" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_INT32 }\n  }\n'),
                    "accept"),
    descriptor_case("d-missing-type-name-reject",
                    root_set('  message_type {\n    name: "M"\n'
                             '    field { name: "a" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_MESSAGE }\n  }\n'),
                    "DIAGNOSTIC_MISSING_TYPE_NAME"),

    # --- INVALID_FIELD_TYPE (the plan-5 follow-up's S1, W-10) --------------
    # The kinds the emitter cannot spell. protoc accepts all of them and
    # emits working code; this project refuses them by decision, so the rows
    # are inverse ones. The UNKNOWN kind (a type outside the TYPE_* set) is
    # NOT a row: protoc's text format will not carry an enumeration value it
    # does not know ("Unknown enumeration value of \"99\" for field \"type\""),
    # and a row's fixture must be protoc's own encoding of a text this
    # project tracks. That shape is measured by the validity lane instead
    # (tests/codegen/validity/data/field-type-unknown.pbs).
    descriptor_case("d-invalid-field-type-accept",
                    root_set('  message_type {\n    name: "M"\n'
                             '    field { name: "a" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_INT32 }\n  }\n'),
                    "accept"),
    descriptor_case("d-invalid-field-type-absent-reject",
                    root_set('  message_type {\n    name: "M"\n'
                             '    field { name: "a" number: 1 label: '
                             'LABEL_OPTIONAL }\n  }\n'),
                    "DIAGNOSTIC_INVALID_FIELD_TYPE", kind="inverse",
                    note=INVALID_FIELD_TYPE_NOTE),

    # --- DUPLICATE_ENUM_NUMBER --------------------------------------------
    descriptor_case("d-duplicate-enum-number-accept",
                    root_set('  enum_type {\n    name: "A"\n'
                             '    options { allow_alias: true }\n'
                             '    value { name: "ZERO" number: 0 }\n'
                             '    value { name: "X" number: 1 }\n'
                             '    value { name: "Y" number: 1 }\n  }\n'),
                    "accept"),
    descriptor_case("d-duplicate-enum-number-reject",
                    root_set('  enum_type {\n    name: "A"\n'
                             '    value { name: "ZERO" number: 0 }\n'
                             '    value { name: "X" number: 1 }\n'
                             '    value { name: "Y" number: 1 }\n  }\n'),
                    "DIAGNOSTIC_DUPLICATE_ENUM_NUMBER"),

    # --- EMPTY_ONEOF ------------------------------------------------------
    descriptor_case("d-empty-oneof-accept",
                    root_set('  message_type {\n    name: "M"\n'
                             '    oneof_decl { name: "pick" }\n'
                             '    field { name: "a" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_INT32 '
                             'oneof_index: 0 }\n  }\n'), "accept"),
    descriptor_case("d-empty-oneof-reject",
                    root_set('  message_type {\n    name: "M"\n'
                             '    oneof_decl { name: "pick" }\n'
                             '    field { name: "a" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_INT32 }\n  }\n'),
                    "DIAGNOSTIC_EMPTY_ONEOF"),

    # --- EMPTY_ENUM (fix round r16, ledger D-32) --------------------------
    descriptor_case("d-empty-enum-accept",
                    root_set(ENUM_E), "accept"),
    descriptor_case("d-empty-enum-reject",
                    root_set('  enum_type {\n    name: "E"\n  }\n'),
                    "DIAGNOSTIC_EMPTY_ENUM"),

    # --- MISSING_IMPORT ---------------------------------------------------
    descriptor_case("d-missing-import-accept",
                    DEP + root_set(
                        '  dependency: "dep.proto"\n  message_type {\n'
                        '    name: "Outer"\n'
                        '    nested_type {\n      name: "Inner"\n'
                        '      field { name: "d" number: 1 label: '
                        'LABEL_OPTIONAL type: TYPE_MESSAGE '
                        'type_name: ".p.D" }\n    }\n  }\n'), "accept"),
    descriptor_case("d-missing-import-reject",
                    file_block("sibling.proto", PLAIN_S) +
                    root_set('  dependency: "dep.proto"\n'
                             '  dependency: "sibling.proto"\n' + PLAIN_M),
                    "DIAGNOSTIC_MISSING_IMPORT"),

    # --- INVALID_IMPORT: the index and duplicate-name rules ---------------
    descriptor_case("d-invalid-import-accept",
                    DEP + root_set('  dependency: "dep.proto"\n' + PLAIN_M),
                    "accept"),
    descriptor_case("d-invalid-import-duplicate-dependency-reject",
                    DEP + root_set('  dependency: "dep.proto"\n'
                                   '  dependency: "dep.proto"\n' + PLAIN_M),
                    "DIAGNOSTIC_INVALID_IMPORT"),
    descriptor_case("d-invalid-import-public-index-accept",
                    DEP + root_set('  dependency: "dep.proto"\n'
                                   '  public_dependency: 0\n' + PLAIN_M),
                    "accept"),
    descriptor_case("d-invalid-import-public-index-out-of-range-reject",
                    DEP + root_set('  dependency: "dep.proto"\n'
                                   '  public_dependency: 3\n' + PLAIN_M),
                    "DIAGNOSTIC_INVALID_IMPORT"),
    descriptor_case("d-invalid-import-public-index-duplicate-inverse",
                    DEP + root_set('  dependency: "dep.proto"\n'
                                   '  public_dependency: 0\n'
                                   '  public_dependency: 0\n' + PLAIN_M),
                    "DIAGNOSTIC_INVALID_IMPORT", kind="inverse",
                    note=D29_NOTE),
    descriptor_case("d-invalid-import-public-and-weak-inverse",
                    DEP + root_set('  dependency: "dep.proto"\n'
                                   '  public_dependency: 0\n'
                                   '  weak_dependency: 0\n' + PLAIN_M),
                    "DIAGNOSTIC_INVALID_IMPORT", kind="inverse",
                    note=D29_NOTE),

    # --- PROTO3 field rules (fix round r17, the 0927-mimo review's F2) -----
    # Two shapes a proto3 file forbids, both refused by protoc when it builds
    # the file from the set (`ValidateProto3Field`, descriptor.cc:7917/7922).
    # The accept twin of each is the PROTO2 control of the same declaration, so
    # a rule that refused the label or the default as such - rather than as a
    # proto3-only shape - fails this lane instead of passing it.
    descriptor_case("d-proto3-required-field-accept",
                    file_block("root.proto",
                               '  message_type {\n    name: "M"\n'
                               '    field { name: "a" number: 1 label: '
                               'LABEL_REQUIRED type: TYPE_INT32 }\n  }\n',
                               syntax="proto2"),
                    "accept"),
    descriptor_case("d-proto3-required-field-reject",
                    root_set('  message_type {\n    name: "M"\n'
                             '    field { name: "a" number: 1 label: '
                             'LABEL_REQUIRED type: TYPE_INT32 }\n  }\n'),
                    "DIAGNOSTIC_PROTO3_REQUIRED_FIELD"),
    descriptor_case("d-proto3-explicit-default-accept",
                    file_block("root.proto",
                               '  message_type {\n    name: "M"\n'
                               '    field { name: "a" number: 1 label: '
                               'LABEL_OPTIONAL type: TYPE_INT32 '
                               'default_value: "5" }\n  }\n',
                               syntax="proto2"),
                    "accept"),
    descriptor_case("d-proto3-explicit-default-reject",
                    root_set('  message_type {\n    name: "M"\n'
                             '    field { name: "a" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_INT32 '
                             'default_value: "5" }\n  }\n'),
                    "DIAGNOSTIC_PROTO3_EXPLICIT_DEFAULT"),

    # --- UNRESOLVED_TYPE: the trailing dot (fix round r17, F7) -------------
    # `split_reference("N.")` leaves an empty remainder, so this text used to
    # be a WHOLE-name reference to `N` and LINKED; protoc walks it as a
    # compound name and refuses it (`"N." is resolved to "p.N.", which is not
    # defined`).
    descriptor_case("d-unresolved-type-trailing-dot-reject",
                    root_set('  message_type {\n    name: "N"\n'
                             '    field { name: "a" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_INT32 }\n  }\n'
                             '  message_type {\n    name: "M"\n'
                             '    field { name: "m" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_MESSAGE '
                             'type_name: "N." }\n  }\n'),
                    "DIAGNOSTIC_UNRESOLVED_TYPE"),

    # --- INVISIBLE_TYPE: the invisible non-type (fix round r17, F8) --------
    # The nearer candidate is an enum value in a file the requester does not
    # import, reached RELATIVELY: it answered NOT_A_TYPE while the absolute
    # spelling of the same declaration answered INVISIBLE - one declaration,
    # two codes. Two files, so the compatibility half is a mode precondition.
    descriptor_case("d-invisible-type-nearer-non-type-reject",
                    root_set('  message_type {\n    name: "M"\n'
                             '    field { name: "f" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_MESSAGE '
                             'type_name: "VALUE" }\n  }\n') +
                    file_block("hidden.proto",
                               '  enum_type {\n    name: "E"\n'
                               '    value { name: "VALUE" number: 0 }\n  }\n'),
                    "DIAGNOSTIC_INVISIBLE_TYPE"),

    # --- INVALID_SYMBOL_NAME: declaration names (fix round r18, D-39) ------
    # protoc's `ValidateSymbolName` (`descriptor.cc:5419-5438`) refuses an empty
    # name ("Missing name.") and any character outside `[A-Za-z0-9_]` ("is not
    # a valid identifier."), at every declaration site; the source path cannot
    # produce either shape. Each pair's accept twin is a valid file of the same
    # origin, so the lane fails if the rule starts refusing legal input.
    descriptor_case("d-invalid-symbol-name-accept",
                    root_set('  message_type {\n    name: "M"\n'
                             '    field { name: "a" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_INT32 }\n  }\n'),
                    "accept"),
    descriptor_case("d-invalid-symbol-name-reject",
                    root_set('  message_type {\n    name: "foo-bar"\n'
                             '    field { name: "a" number: 1 label: '
                             'LABEL_OPTIONAL type: TYPE_INT32 }\n  }\n'),
                    "DIAGNOSTIC_INVALID_SYMBOL_NAME"),
    descriptor_case("d-empty-symbol-name-accept",
                    root_set('  enum_type {\n    name: "E"\n'
                             '    value { name: "ZERO" number: 0 }\n  }\n'),
                    "accept"),
    descriptor_case("d-empty-symbol-name-reject",
                    root_set('  enum_type {\n    name: ""\n'
                             '    value { name: "ZERO" number: 0 }\n  }\n'),
                    "DIAGNOSTIC_INVALID_SYMBOL_NAME"),
] + EXTRA_CASES


def encode(text):
    """protoc's own encoding of an authored FileDescriptorSet text."""
    encode_dir = tempfile.mkdtemp(prefix="easypb-parity-encode-")
    try:
        done = subprocess.run(
            [PROTOC, "--proto_path=" + SRC,
             "--encode=google.protobuf.FileDescriptorSet", DESCRIPTOR_PROTO],
            cwd=encode_dir, input=text.encode("utf-8"), capture_output=True)
        if done.returncode != 0:
            raise SystemExit("encode failed: " +
                             (done.stderr or b"").decode("utf-8", "replace"))
        return done.stdout
    finally:
        shutil.rmtree(encode_dir, ignore_errors=True)


def source_verdict(case, scratch):
    for rel, text in case["files"].items():
        write(os.path.join("source", rel), text)
    cmd = [PROTOC, "--proto_path=" + SOURCE,
           "--descriptor_set_out=" + os.path.join(scratch,
                                                  case["name"] + ".pb")]
    cmd += [os.path.join(SOURCE, rel) for rel in case["roots"]]
    return run(cmd, scratch)


def descriptor_verdict(case, scratch):
    """A13: --encode the textproto, then build FROM the set, no --proto_path.

    The build runs in an empty scratch directory on purpose: protoc falls back
    to the working directory for an unqualified name, so a `root.proto` beside
    the invocation would be recompiled from source and the set never read
    (D-24's counter-example).
    """
    txtpb = write(os.path.join("set", case["name"] + ".txtpb"), case["text"])
    pbs = os.path.join(SETS, case["name"] + ".pbs")
    io.open(pbs, "wb").write(encode(case["text"]))
    build_dir = tempfile.mkdtemp(prefix="easypb-parity-build-")
    try:
        out = os.path.join(build_dir, "cpp")
        os.makedirs(out)
        return run([PROTOC, "--descriptor_set_in=" + pbs,
                    "--cpp_out=" + out, "root.proto"], build_dir) + (pbs,)
    finally:
        shutil.rmtree(build_dir, ignore_errors=True)


def first_line(text):
    for line in text.splitlines():
        line = line.strip()
        if line:
            return line
    return ""


def main(argv=None):
    global PROTOC, SRC, DESCRIPTOR_PROTO

    parser = argparse.ArgumentParser(
        description="Writes the two-way oracle parity corpus in place: every "
                    "fixture under source/ and set/, and parity-cases.tsv.")
    parser.add_argument("--protoc", default=PROTOC,
                        help="path to the official protoc executable "
                             "(default: %(default)s)")
    parser.add_argument("--descriptor-proto-path", default=None,
                        help="a proto path holding descriptor.proto (default: "
                             "the reference tree under build/, the protoc "
                             "release's include/, or the tracked differential "
                             "copy, in that order)")
    args = parser.parse_args(argv)

    PROTOC = args.protoc
    SRC, DESCRIPTOR_PROTO = find_descriptor_proto(PROTOC,
                                                  args.descriptor_proto_path)
    if not os.path.exists(os.path.join(SRC, DESCRIPTOR_PROTO)):
        if args.descriptor_proto_path:
            # Name the spelling the caller typed next to the resolution, never
            # only a path it did not write (the generator's cwd is the corpus).
            print("no %s under %s (--descriptor-proto-path was given as "
                  "'%s'): pass --descriptor-proto-path"
                  % (DESCRIPTOR_PROTO, SRC, args.descriptor_proto_path))
        else:
            print("no %s under %s: pass --descriptor-proto-path"
                  % (DESCRIPTOR_PROTO, SRC))
        return 2

    for path in (SOURCE, SETS, os.path.dirname(LOG)):
        if not os.path.isdir(path):
            os.makedirs(path)
    for rel, text in SHARED.items():
        write(os.path.join("source", rel), text)

    scratch = tempfile.mkdtemp(prefix="easypb-parity-scratch-")
    rows = []
    try:
        for case in SOURCE_CASES:
            code, text = source_verdict(case, scratch)
            case["protoc"] = "accept" if code == 0 else "reject"
            case["first"] = first_line(text)
            rows.append(case)
        for case in DESCRIPTOR_CASES:
            code, text, unused = descriptor_verdict(case, scratch)
            case["protoc"] = "accept" if code == 0 else "reject"
            case["first"] = first_line(text)
            rows.append(case)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    # The tsv: the row table both readers consume (§11.3).
    tsv = ["case\torigin\tprotoc\texpected\tkind\tnote"]
    for row in rows:
        if row.get("no_row"):
            continue  # measured evidence, deliberately not a row
        tsv.append("\t".join([row["name"], row["origin"], row["protoc"],
                              row["linker"], row["kind"], row["note"]]))
    io.open(TSV, "w", encoding="utf-8", newline="\n").write("\n".join(tsv) +
                                                            "\n")

    log = ["# Step 11 - the protoc half of every parity fixture", "",
           "Measured with libprotoc 30.1 by `generate.py`; the fixture",
           "text is the authority, the `.pbs` bytes are protoc's own encoding",
           "of the authored textproto (A13: --encode, then",
           "--descriptor_set_in with no --proto_path, run from an empty",
           "directory). `linker` is the row's hypothesis, measured by",
           "`test_parity.cpp`; the expectation and the two verdicts must agree",
           "before the lane is green.", "",
           "| Case | Origin | protoc | expected | first output line |",
           "| --- | --- | --- | --- | --- |"]
    for row in rows:
        log.append("| `%s` | %s | %s | %s | %s |" % (
            row["name"], row["origin"], row["protoc"],
            "no row" if row.get("no_row") else row["linker"],
            ("`" + row["first"] + "`") if row["first"] else "-"))
    io.open(LOG, "w", encoding="utf-8", newline="\n").write("\n".join(log) +
                                                            "\n")

    for row in rows:
        print("%-52s %-10s protoc=%-6s expected=%-30s %s" % (
            row["name"], row["origin"], row["protoc"], row["linker"],
            row["first"][:60]))
    accepts = sum(1 for r in rows if r["protoc"] == "accept")
    print("\ncases %d, protoc accept %d, protoc reject %d" % (
        len(rows), accepts, len(rows) - accepts))
    print("written: " + TSV)
    print("written: " + LOG)
    return 0


if __name__ == "__main__":
    sys.exit(main())
