#!/usr/bin/env python3
"""descriptor_fixtures.py - the shared `.pbs` fixture builder.

Five lanes - `library`, `limits`, `names`, `parity` and `validity` - ship
committed `.pbs` descriptor sets and a `data/make_descriptors.py` that rebuilds
them. Before this module each script carried its own copy of the same four
mechanics: the protoc lookup, the `descriptor.proto` lookup, the text-format
encoder and the file writer. They live here once, so a lane script is a table
of fixtures and every lane encodes a byte the same way.

A fixture is one of:

- `Fixture.compile(stem, ...)` - protoc builds `<stem>.pbs` from `<source>.proto`
  with `--descriptor_set_out`. This is the ordinary case: the descriptor is
  protoc's own encoding of the source beside it.
- `Fixture.craft(stem, ...)` - `<stem>.pbs` is encoded from `<stem>.txtpb` with
  `--encode=google.protobuf.FileDescriptorSet`, because protoc refuses or
  canonicalizes that shape when it comes from source text. An optional `patch`
  rewrites the encoded bytes for the one fixture whose single byte the text
  format cannot spell (`validity`'s `type: 99`).
- `Fixture.source_only(stem, ...)` - a source fixture with no descriptor half.
  Nothing is compiled; the entry exists so the generator reports the skip
  instead of silently forgetting the file.

Both modes can carry `source_text`: the generated shapes in `limits`
(`nesting-31`, `nesting-32`, `dependency-chain`) are not writable by hand, so
the generator writes the `.proto` first and then produces its `.pbs` half. The
result is still the only producer of both halves.

`build_fixtures(lane_dir, table)` builds the table and then every `.txtpb` in
`lane_dir` that the table did not name, so adding a text-format fixture cannot
be forgotten and a `.txtpb` always wins over the `.proto` of the same stem.
`parse_args()` adds `--protoc` and `--check`; with `--check` every fixture is
built in memory and compared with the committed file instead of written:

    python tests/codegen/<lane>/data/make_descriptors.py --check

That is the byte-identity gate: a lane's committed fixtures are exactly what
this generator produces from the checked-in `.proto` and `.txtpb` sources.

`descriptor.proto` is looked up in this order:

    1. $EASYPB_DESCRIPTOR_PROTO (a directory holding google/protobuf/descriptor.proto)
    2. build/protobuf-30.1-src/protobuf-30.1/src (the repo-local scratch copy)
    3. tests/codegen/parser/differential/corpus (the flat copy the repository
       already tracks for the differential lane)

`protoc` is `--protoc`, then `$EASYPB_PROTOC`, then the pinned 30.1 oracle, then
`protoc` on PATH. Running a lane never needs protoc: the fixtures are committed.

Two generators beside these five stay on their own code on purpose:

- `tests/codegen/cli/data/make_fixtures.py` writes raw wire bytes with a minimal
  varint writer: its fixtures have no `FileDescriptorProto.name` (or a
  case-only name pair), which protoc never emits, so there is no text-format
  shape to encode and no `.proto` half to compile.
- `tests/codegen/imports/data/parity/generate.py` is a corpus builder, not a
  descriptor encoder: it writes `.proto`, `.txtpb` and `.pbs` files plus the
  `parity-cases.tsv` table, measures protoc's verdict for every row and uses
  the corpus as its working directory.
"""

import argparse
import io
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PROTOC = r"C:\Base\Tools\BIN\protoc.exe"

COMPILED = "compiled"
CRAFTED = "crafted"
SOURCE_ONLY = "source-only"


def repo_root():
    """The checkout root, derived from this file's location."""
    return os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


DESCRIPTOR_DIRS = (
    os.path.join(repo_root(), "build", "protobuf-30.1-src", "protobuf-30.1",
                 "src"),
    os.path.join(repo_root(), "tests", "codegen", "parser", "differential",
                 "corpus"),
)


def fail(message):
    sys.stderr.write(message + "\n")
    raise SystemExit(1)


def find_protoc(explicit):
    if explicit:
        return explicit
    from_env = os.environ.get("EASYPB_PROTOC")
    if from_env:
        return from_env
    if os.path.exists(DEFAULT_PROTOC):
        return DEFAULT_PROTOC
    return "protoc"


def find_descriptor_dir():
    from_env = os.environ.get("EASYPB_DESCRIPTOR_PROTO")
    if from_env:
        if os.path.exists(os.path.join(from_env, "google", "protobuf",
                                       "descriptor.proto")):
            return from_env
        fail("EASYPB_DESCRIPTOR_PROTO=%s holds no google/protobuf/descriptor.proto"
             % from_env)
    for candidate in DESCRIPTOR_DIRS:
        if os.path.exists(os.path.join(candidate, "google", "protobuf",
                                       "descriptor.proto")):
            return candidate
        if os.path.exists(os.path.join(candidate, "descriptor.proto")):
            return candidate
    fail("no google/protobuf/descriptor.proto found; set "
         "EASYPB_DESCRIPTOR_PROTO to the directory that holds it")


def run(command, stdin_bytes=None):
    result = subprocess.run(command, input=stdin_bytes, capture_output=True)
    if result.returncode != 0:
        fail("command failed (%d): %s\n%s"
             % (result.returncode, " ".join(command),
                result.stderr.decode("utf-8", "replace")))
    return result.stdout


def encode_textproto(protoc, descriptor_dir, text):
    """Encode `text` as a `google.protobuf.FileDescriptorSet`."""
    return run([protoc, "--encode=google.protobuf.FileDescriptorSet",
                "--proto_path=" + descriptor_dir,
                "google/protobuf/descriptor.proto"], text)


def compile_proto(protoc, source, output, include_dirs=(), flags=()):
    """Compile `source` into `output` with `--descriptor_set_out`."""
    command = [protoc]
    for directory in include_dirs:
        command.append("--proto_path=" + directory)
    command.append("--descriptor_set_out=" + output)
    command.extend(flags)
    command.append(source)
    run(command)


def read_bytes(path):
    try:
        with io.open(path, "rb") as handle:
            return handle.read()
    except OSError as error:
        fail("cannot read %s: %s" % (path, error))


def write(path, data, check=False):
    """Write `data` to `path`, or with `check` prove the file already holds it."""
    name = os.path.basename(path)
    if check:
        current = read_bytes(path)
        if current != data:
            first = next((index for index, (old, new)
                          in enumerate(zip(current, data)) if old != new),
                         min(len(current), len(data)))
            fail("%s is stale: the generator would rewrite it (%d bytes "
                 "committed, %d generated, first difference at byte %d)"
                 % (name, len(current), len(data), first))
        print("unchanged %s (%d bytes)" % (name, len(data)))
        return
    with io.open(path, "wb") as handle:
        handle.write(data)
    print("wrote %s (%d bytes)" % (name, len(data)))


def parse_args(description):
    """The common CLI: `--protoc` and the `--check` byte-identity gate."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--protoc", default="", help="the protoc executable")
    parser.add_argument("--check", action="store_true",
                        help="verify the committed fixtures instead of "
                             "writing them")
    return parser.parse_args()


class Fixture(object):
    """One fixture in a lane's `data/` directory.

    Use the `compile`, `craft` or `source_only` constructor rather than the
    class itself: the kind decides how the `.pbs` bytes are produced.
    """

    def __init__(self, kind, stem, source=None, source_text=None, text=None,
                 patch=None, include_dirs=(), flags=()):
        if kind not in (COMPILED, CRAFTED, SOURCE_ONLY):
            raise ValueError("unknown fixture kind %r" % (kind,))
        if not stem:
            raise ValueError("a fixture needs a stem")
        if kind != COMPILED and (source is not None or include_dirs or flags):
            raise ValueError("%s: only a compiled fixture names a source stem "
                             "or protoc flags" % stem)
        if kind != CRAFTED and (text is not None or patch is not None):
            raise ValueError("%s: only a crafted fixture carries text or a "
                             "patch" % stem)
        self.kind = kind
        self.stem = stem
        self.source = source or stem
        self.source_text = source_text
        self.text = text
        self.patch = patch
        self.include_dirs = tuple(include_dirs)
        self.flags = tuple(flags)

    @classmethod
    def compile(cls, stem, source=None, source_text=None, include_dirs=(),
                flags=()):
        """A `.pbs` protoc builds from `<source>.proto`."""
        return cls(COMPILED, stem, source=source, source_text=source_text,
                   include_dirs=include_dirs, flags=flags)

    @classmethod
    def craft(cls, stem, text=None, patch=None, source_text=None):
        """A `.pbs` encoded from `<stem>.txtpb` (or from `text`)."""
        return cls(CRAFTED, stem, source_text=source_text, text=text,
                   patch=patch)

    @classmethod
    def source_only(cls, stem, source_text=None):
        """A `.proto` fixture with no descriptor half."""
        return cls(SOURCE_ONLY, stem, source_text=source_text)

    def build(self, lane_dir, protoc, descriptor_dir, check=False):
        if self.source_text is not None:
            write(os.path.join(lane_dir, self.stem + ".proto"),
                  self.source_text, check)
        if self.kind == SOURCE_ONLY:
            print("skipped %s.proto (source-only)" % self.source)
            return
        target = os.path.join(lane_dir, self.stem + ".pbs")
        if self.kind == CRAFTED:
            text = self.text
            if text is None:
                text = read_bytes(os.path.join(lane_dir,
                                               self.stem + ".txtpb"))
            if self.patch is not None:
                data = self.patch(protoc, descriptor_dir, text)
            else:
                data = encode_textproto(protoc, descriptor_dir, text)
        else:
            data = _compile_bytes(lane_dir, self, protoc)
        write(target, data, check)


def _compile_bytes(lane_dir, fixture, protoc):
    """Compile one fixture into a scratch file and return its bytes."""
    with tempfile.TemporaryDirectory(prefix="easypb-fixture-") as scratch:
        target = os.path.join(scratch, fixture.stem + ".pbs")
        compile_proto(protoc, fixture.source + ".proto", target,
                      (lane_dir,) + fixture.include_dirs, fixture.flags)
        return read_bytes(target)


def build_fixtures(lane_dir, table=None, source_only=(), patches=None,
                   check=False, protoc=""):
    """Build a lane's fixtures; with `check`, prove the committed bytes.

    `table` is a sequence of `Fixture` objects. When it is omitted, every
    `.proto` in `lane_dir` is compiled except the `source_only` stems and the
    stems that have a `.txtpb` beside them - the `names` and `validity` shape.

    Every `.txtpb` in `lane_dir` that the table does not already name is
    crafted, so a text-format fixture cannot be forgotten; a `.txtpb` therefore
    always wins over the `.proto` of the same stem. `patches` maps a stem to a
    `callable(protoc, descriptor_dir, text) -> bytes` that post-processes the
    encoded descriptor, and `source_only` names `.proto` fixtures whose lane
    never wants a descriptor half.

    With `check`, nothing is written: every generated `.proto` and `.pbs` is
    compared with the committed file, and the first difference is fatal.
    """
    lane_dir = os.path.abspath(lane_dir)
    if not os.path.isdir(lane_dir):
        fail("%s is not a directory" % lane_dir)
    protoc = find_protoc(protoc)
    descriptor_dir = find_descriptor_dir()
    patches = dict(patches or {})

    names = sorted(os.listdir(lane_dir))
    txtpb = [name[:-len(".txtpb")] for name in names
             if name.endswith(".txtpb")]
    protos = [name[:-len(".proto")] for name in names
              if name.endswith(".proto")]

    for stem in source_only:
        if stem not in protos:
            fail("source-only fixture %s has no %s.proto" % (stem, stem))
    for stem in patches:
        if stem not in txtpb:
            fail("patched fixture %s has no %s.txtpb" % (stem, stem))

    if table is None:
        entries = [Fixture.compile(stem) for stem in protos
                   if stem not in source_only and stem not in txtpb]
    else:
        entries = list(table)

    declared = {}
    for fixture in entries:
        if fixture.stem in declared:
            fail("%s is declared twice in the fixture table" % fixture.stem)
        if fixture.stem in txtpb and fixture.kind != CRAFTED:
            fail("%s: a .txtpb exists, so the descriptor is crafted, not %s"
                 % (fixture.stem, fixture.kind))
        if fixture.stem in source_only and fixture.kind != SOURCE_ONLY:
            fail("%s: declared source-only, so it cannot be %s"
                 % (fixture.stem, fixture.kind))
        declared[fixture.stem] = fixture

    for stem in txtpb:
        if stem not in declared:
            entries.append(Fixture.craft(stem, patch=patches.get(stem)))
    for stem in source_only:
        if stem not in declared:
            entries.append(Fixture.source_only(stem))

    for fixture in entries:
        fixture.build(lane_dir, protoc, descriptor_dir, check)
    return 0
