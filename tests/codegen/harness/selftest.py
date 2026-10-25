"""Unit checks for the harness decision layer (no product code involved).

Run with:
    python tests/codegen/harness/selftest.py
"""

from __future__ import annotations

import json
import os
import pathlib
import sys
import tempfile
import unittest

import compare
import compile as p3
import fds
import mutate
import runner
import seed
import shrink
import wire

HERE = pathlib.Path(__file__).resolve().parent
REPO = runner.repo_root(HERE)
CATALOGUE = REPO / "build/verification/property-catalogue.md"
PARITY_TSV = (REPO / "tests/codegen/imports/data/parity"
              / "parity-cases.tsv")


def fake_result(files=None, rc=0, stdout=b"", stderr=b"", timed_out=False):
    return runner.ExecResult(
        argv=["tool"], cwd=".", rc=rc, timed_out=timed_out, duration_s=0.0,
        stdout=stdout, stderr=stderr, files=files or [],
    )


def file_info(path, size=10, sha="a" * 64):
    return runner.FileInfo(path, size, sha)


class FdsTest(unittest.TestCase):
    def test_target_prefers_root_and_dependencies(self):
        data = (
            b"\x0a\x02\x0a\x00"  # file with empty name
        )
        self.assertIsNone(fds.select_target(fds.file_descriptors(data)))

        def file(name, deps=()):
            body = b"\x0a" + bytes([len(name)]) + name.encode()
            for dep in deps:
                body += b"\x1a" + bytes([len(dep)]) + dep.encode()
            return b"\x0a" + bytes([len(body)]) + body

        files = fds.file_descriptors(file("dep.proto") + file("root.proto", ["dep.proto"]))
        self.assertEqual([item.name for item in files], ["dep.proto", "root.proto"])
        self.assertEqual(fds.select_target(files), "root.proto")

    def test_header_stem(self):
        self.assertEqual(fds.header_stem("a/b/c.proto"), "c")
        self.assertEqual(fds.header_stem("x.proto3"), "x.proto3")


class VerdictTest(unittest.TestCase):
    def test_codegen_requires_rc_and_nonempty_header(self):
        result = fake_result([file_info("x.pb.hpp")])
        self.assertTrue(compare.codegen_verdict(result, "x.proto").accept)
        self.assertFalse(compare.codegen_verdict(
            fake_result([file_info("x.pb.hpp")], rc=1), "x.proto").accept)
        self.assertFalse(compare.codegen_verdict(
            fake_result([file_info("x.pb.hpp", size=0)]), "x.proto").accept)
        # A single generated header is the accepted artefact even when its
        # stem differs; P5 (not P1) owns target-name correctness.
        self.assertTrue(compare.codegen_verdict(
            fake_result([file_info("other.pb.hpp")]), "x.proto").accept)
        self.assertFalse(compare.codegen_verdict(
            fake_result([file_info("a.pb.hpp"), file_info("b.pb.hpp")]),
            "x.proto").accept)

    def test_protoc_accepts_header_or_source(self):
        self.assertTrue(compare.protoc_verdict(
            fake_result([file_info("x.pb.h")]), "x.proto").accept)
        self.assertTrue(compare.protoc_verdict(
            fake_result([file_info("x.pb.cc")]), "x.proto").accept)
        self.assertFalse(compare.protoc_verdict(
            fake_result([file_info("x.pb.h")], timed_out=True), "x.proto").accept)


class P1Test(unittest.TestCase):
    def test_parity(self):
        row = {"root": None}
        check = compare.check_p1(row, compare.Verdict(True, 0, False),
                                 compare.Verdict(True, 0, False))
        self.assertTrue(check["match"])
        self.assertEqual(check["classification"], "pass")

    def test_known_root(self):
        check = compare.check_p1({"root": "R3"}, compare.Verdict(True, 0, False),
                                 compare.Verdict(False, 1, False))
        self.assertFalse(check["match"])
        self.assertEqual(check["classification"], "known")
        self.assertEqual(check["root"], "R3")

    def test_new_root(self):
        check = compare.check_p1({"root": None}, compare.Verdict(False, 1, False),
                                 compare.Verdict(True, 0, False))
        self.assertEqual(check["classification"], "new")

    def test_allowed_stricter(self):
        check = compare.check_p1(
            {"root": None}, compare.Verdict(False, 1, False),
            compare.Verdict(True, 0, False),
            "x: recursive message value dependency: A -> A",
        )
        self.assertEqual(check["classification"], "allowed-stricter")
        self.assertEqual(check["policy"], "P1-policy/by-value-recursion")

    def test_presence_flag_policy(self):
        check = compare.check_p1(
            {"root": None}, compare.Verdict(False, 1, False),
            compare.Verdict(True, 0, False),
            "x: The field 'has_foo' of message '.M' collides with the presence "
            "flag of the field 'foo': both members would be named 'has_foo'",
        )
        self.assertEqual(check["classification"], "allowed-stricter")
        self.assertEqual(check["policy"], "P1-policy/presence-flag-collision")

    def test_false_positive_marker(self):
        row = {"root": None, "false_positives": ["P1"]}
        check = compare.check_p1(row, compare.Verdict(True, 0, False),
                                 compare.Verdict(False, 1, False))
        self.assertEqual(check["classification"], "false-positive")


class P2Test(unittest.TestCase):
    def test_equal_hashes(self):
        source = compare.Verdict(True, 0, False, compare.Artifact("x.pb.hpp", 10, "aa"))
        descriptor = compare.Verdict(True, 0, False, compare.Artifact("x.pb.hpp", 10, "aa"))
        check = compare.check_p2({"root": None}, source, descriptor, None, None)
        self.assertTrue(check["match"])
        self.assertEqual(check["classification"], "pass")

    def test_different_hashes(self):
        source = compare.Verdict(True, 0, False, compare.Artifact("x.pb.hpp", 10, "aa"))
        descriptor = compare.Verdict(True, 0, False, compare.Artifact("x.pb.hpp", 10, "bb"))
        check = compare.check_p2({"root": "R1"}, source, descriptor, None, None)
        self.assertFalse(check["match"])
        self.assertEqual(check["classification"], "known")
        self.assertEqual(check["root"], "R1")

    def test_descriptor_refusal_is_a_mismatch(self):
        source = compare.Verdict(True, 0, False, compare.Artifact("x.pb.hpp", 10, "aa"))
        descriptor = compare.Verdict(False, 1, False)
        check = compare.check_p2({"root": None}, source, descriptor, None, None)
        self.assertTrue(check["applicable"])
        self.assertFalse(check["match"])
        self.assertEqual(check["classification"], "new")

    def test_source_refusal_is_not_applicable(self):
        source = compare.Verdict(False, 1, False)
        check = compare.check_p2({"root": None}, source,
                                 compare.Verdict(True, 0, False), None, None)
        self.assertFalse(check["applicable"])
        self.assertEqual(check["classification"], "n/a")

    def test_first_difference(self):
        with tempfile.TemporaryDirectory() as directory:
            left = pathlib.Path(directory) / "left.hpp"
            right = pathlib.Path(directory) / "right.hpp"
            left.write_text("a\nb\nc\n", encoding="utf-8")
            right.write_text("a\nB\nc\n", encoding="utf-8")
            diff = compare.first_differing_line(str(left), str(right))
            self.assertEqual(diff["line"], 2)
            self.assertEqual(diff["source"], "b")
            self.assertEqual(diff["descriptor"], "B")


class WireTest(unittest.TestCase):
    def test_file_descriptor_round_trip(self):
        payload = wire.fds([wire.file_descriptor(
            name="root.proto", package="p", syntax="proto3",
            messages=[wire.message_type("M", fields=[
                wire.field("a", 1),
                wire.field("e", 2, type=wire.TYPE_ENUM,
                           type_name=".q.E", default="Z"),
            ])],
        )])
        files = fds.file_descriptors(payload)
        self.assertEqual([item.name for item in files], ["root.proto"])
        self.assertEqual(files[0].package, "p")
        self.assertEqual(files[0].syntax, "proto3")
        # The mutator's own parser must read every hand-built shape.
        root = mutate.parse(payload)
        self.assertEqual(mutate.base_target(payload), "root.proto")
        self.assertTrue(root.encode())

    def test_hand_built_shapes_are_parseable(self):
        self.assertEqual(len(mutate.HAND_BUILT_ORDER),
                         len(set(mutate.HAND_BUILT_ORDER)))
        for shape in mutate.HAND_BUILT_ORDER:
            mutation = mutate.hand_built(shape)
            self.assertEqual(mutation.operation, shape)
            self.assertTrue(mutation.payload)
            files = fds.file_descriptors(mutation.payload)
            self.assertTrue(files, shape)
            self.assertIn(mutation.target, [item.name for item in files], shape)

    def test_edition_field_is_written(self):
        payload = mutate.hb_editions_file().payload
        files = fds.file_descriptors(payload)
        self.assertEqual(files[0].syntax, "editions")
        self.assertIn(b"\x70\xe8\x07", payload)  # field 14, edition 1000


class InverseRowsTest(unittest.TestCase):
    def test_ten_inverse_rows_have_an_exception(self):
        cases = []
        for line in PARITY_TSV.read_text(encoding="utf-8").splitlines():
            if not line or line.startswith("case\t"):
                continue
            fields = line.split("\t")
            if fields[4] == "inverse":
                cases.append(fields[0])
        self.assertEqual(len(cases), 10)
        self.assertEqual(set(cases), set(seed.INVERSE_EXCEPTIONS))
        catalogue = CATALOGUE.read_text(encoding="utf-8")
        for case, exception in seed.INVERSE_EXCEPTIONS.items():
            self.assertTrue(case)
            self.assertIn("| %s |" % exception, catalogue, case)

    def test_corpus_carries_the_inverse_rows_as_exceptions(self):
        rows = {}
        for path in (HERE / "corpus" / "rows").glob("*.expect.json"):
            row = json.loads(path.read_text(encoding="utf-8"))
            rows[row["id"]] = row
        for case, exception in seed.INVERSE_EXCEPTIONS.items():
            row = rows["t-parity-" + case]
            self.assertEqual(row["exception"], exception)
            self.assertNotEqual(row["kind"], "not-comparable")
            self.assertEqual(row["p1_expect"], "stricter")


class CompileClassificationTest(unittest.TestCase):
    def test_cr05_signature_is_a_known_r1_instance(self):
        verdict = {"rc": 1, "first_error":
                   "case.pb.hpp:28:22: error: integer constant is so large "
                   "that it is unsigned", "stderr_tail": ""}
        self.assertEqual(
            p3.classify_failure({"root": "R1"}, "cxx11", verdict),
            "known-root-instance:CR-05")
        self.assertEqual(
            p3.classify_failure({"root": "R5", "cr": None}, "cxx11", verdict),
            "unexplained")
        self.assertEqual(
            p3.classify_failure({"cr": "CR-05"}, "cxx11", verdict),
            "known-p3-row")

    def test_missing_dependency_is_a_setup_gap(self):
        verdict = {"rc": 1, "first_error": None,
                   "stderr_tail": "x.pb.hpp:1:10: fatal error: dep.pb.hpp: "
                                  "No such file or directory"}
        self.assertEqual(
            p3.classify_failure({"root": "R1"}, "cxx11", verdict),
            "missing-dependency-header")


class ShrinkTest(unittest.TestCase):
    def test_split_proto_keeps_braces_together(self):
        text = ('syntax = "proto2";\n'
                "message M {\n"
                "  optional int32 a = 1;\n"
                "  message N { optional int32 b = 2; }\n"
                "}\n"
                "option (x) = { y: 1 };\n")
        units = shrink.split_proto(text)
        self.assertEqual(len(units), 3)
        self.assertTrue(units[1].startswith("message M"))
        self.assertEqual(units[1].count("{"), units[1].count("}"))
        head, body, tail = shrink.split_body(units[1])
        self.assertEqual(head, "message M {")
        self.assertEqual(tail, "}")
        self.assertEqual(len(body), 2)

    def test_normalise_drops_paths_and_positions(self):
        left = ("C:\\w\\e0001\\case.proto:1:1: Expected top-level statement")
        right = ("C:\\w\\e0042\\case.proto:9:3: Expected top-level statement")
        self.assertEqual(shrink._normalise(left), shrink._normalise(right))
        self.assertEqual(
            shrink._normalise("root.proto: field at [4, 0, 2, 1]"),
            shrink._normalise("root.proto: field at [4, 0, 9, 7]"))


class RunnerTest(unittest.TestCase):
    def test_argv_without_shell(self):
        with tempfile.TemporaryDirectory() as directory:
            result = runner.execute(
                [sys.executable, "-c", "print('ok')"], directory, timeout_s=30
            )
            self.assertEqual(result.rc, 0)
            self.assertEqual(result.stdout.strip(), b"ok")
            self.assertFalse(result.timed_out)
            self.assertFalse(result.crashed)

    def test_timeout(self):
        with tempfile.TemporaryDirectory() as directory:
            result = runner.execute(
                [sys.executable, "-c", "import time; time.sleep(10)"],
                directory, timeout_s=0.3,
            )
            self.assertTrue(result.timed_out)
            self.assertIsNone(result.rc)

    def test_capture_file_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            out = os.path.join(directory, "out")
            os.makedirs(out)
            with open(os.path.join(out, "x.pb.hpp"), "wb") as handle:
                handle.write(b"hello")
            result = runner.execute(
                [sys.executable, "-c", "pass"], directory, timeout_s=30, watch_dir=out
            )
            self.assertEqual(len(result.files), 1)
            self.assertEqual(result.files[0].path, "x.pb.hpp")
            self.assertEqual(result.files[0].size, 5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
