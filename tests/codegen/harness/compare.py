"""P1/P2 comparator for the harness pilot.

P1 — acceptance parity with protoc: ``codegen accepts I <=> protoc accepts I``.
P2 — cross-path identity: the header generated from a source tree has the same
SHA-256 as the header generated from the protoc-written FileDescriptorSet.

The comparator reads only the captured runner results; it never invokes a tool
itself.  Every case gets one verdict per property, and every divergence is
classified as a known root, an exception, a false positive or a new root.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field

from fds import header_stem
from runner import ExecResult


@dataclass
class Artifact:
    path: str | None = None
    size: int = 0
    sha256: str | None = None

    def as_dict(self) -> dict:
        return {"path": self.path, "size": self.size, "sha256": self.sha256}


@dataclass
class Verdict:
    accept: bool
    rc: int | None
    timed_out: bool
    artifact: Artifact = field(default_factory=Artifact)

    def as_dict(self) -> dict:
        return {
            "accept": self.accept,
            "rc": self.rc,
            "timed_out": self.timed_out,
            "artifact": self.artifact.as_dict(),
        }


def _pick_artifact(result: ExecResult, suffix: str, stem: str) -> Artifact:
    exact = [item for item in result.files
             if item.path.rsplit("/", 1)[-1] == stem + suffix]
    if exact:
        # Prefer the header at the shallowest path; a target with a logical
        # directory may be written below it by protoc.
        chosen = min(exact, key=lambda item: item.path.count("/"))
        return Artifact(chosen.path, chosen.size, chosen.sha256)
    if len(result.files) == 1 and result.files[0].path.endswith(suffix):
        item = result.files[0]
        return Artifact(item.path, item.size, item.sha256)
    return Artifact()


def codegen_verdict(result: ExecResult, logical_name: str) -> Verdict:
    stem = header_stem(logical_name)
    artifact = _pick_artifact(result, ".pb.hpp", stem)
    accept = (
        not result.timed_out
        and result.rc == 0
        and artifact.sha256 is not None
        and artifact.size > 0
    )
    return Verdict(accept, result.rc, result.timed_out, artifact)


def protoc_verdict(result: ExecResult, logical_name: str) -> Verdict:
    stem = header_stem(logical_name)
    header = _pick_artifact(result, ".pb.h", stem)
    source = _pick_artifact(result, ".pb.cc", stem)
    artifact = header if header.sha256 is not None else source
    accept = (
        not result.timed_out
        and result.rc == 0
        and artifact.sha256 is not None
        and artifact.size > 0
    )
    return Verdict(accept, result.rc, result.timed_out, artifact)


def first_differing_line(
    left_path: str | None, right_path: str | None
) -> dict | None:
    """Return the first differing line of two generated headers, if readable."""
    if not left_path or not right_path:
        return None
    if not (os.path.isfile(left_path) and os.path.isfile(right_path)):
        return None
    try:
        with open(left_path, "rb") as handle:
            left = handle.read().decode("utf-8", "replace").splitlines()
        with open(right_path, "rb") as handle:
            right = handle.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return None
    for number, (a, b) in enumerate(zip(left, right), start=1):
        if a != b:
            return {"line": number, "source": a, "descriptor": b}
    if len(left) != len(right):
        number = min(len(left), len(right)) + 1
        return {
            "line": number,
            "source": left[number - 1] if number <= len(left) else "<end of file>",
            "descriptor": right[number - 1] if number <= len(right) else "<end of file>",
        }
    return None


def classify(row: dict, property_name: str, applicable: bool, match: bool) -> str:
    """One of: n/a, pass, exception, known, false-positive, new."""
    if not applicable:
        return "n/a"
    if match:
        return "pass"
    false_positives = row.get("false_positives") or []
    if property_name in false_positives:
        return "false-positive"
    if property_name == "P2" and row.get("p2_exception"):
        return "exception"
    if row.get("exception"):
        return "exception"
    p2_root = row.get("p2_root") if property_name == "P2" else None
    if p2_root or row.get("root"):
        return "known"
    return "new"


# Stricter-than-protoc refusals that the P1 policy explicitly allows: accepting
# the input would emit invalid C++.  Each entry is (stderr marker, policy id,
# authority).  This is a policy check, not a per-row exception: the same marker
# classifies every input that triggers it.
ALLOWED_STRICTER = [
    (
        "recursive message value dependency",
        "P1-policy/by-value-recursion",
        "GENERATED_CODE.md: message fields are C++ values; a by-value recursive "
        "graph has no finite type",
    ),
    (
        "would both be named",
        "P1-policy/non-injective-name-escape",
        "names lane PC-02: the escape rule is not injective; refusing is the "
        "only way to avoid two identical C++ names",
    ),
    (
        "collides with the presence flag",
        "P1-policy/presence-flag-collision",
        "GENERATED_CODE.md: a presence flag that collides with a member of its "
        "own message is refused; the reference compiler accepts the shape and "
        "emits a header that does not compile",
    ),
    (
        "is not a valid C++ identifier",
        "P1-policy/non-identifier-name",
        "B6 / property-catalogue.md P1 policy: a descriptor-origin name that "
        "is not a C++ identifier would emit an invalid declaration; protoc "
        "accepts the descriptor shape",
    ),
]


def find_allowed_stricter(stderr: str) -> dict | None:
    for marker, policy, authority in ALLOWED_STRICTER:
        if marker in stderr:
            return {"policy": policy, "authority": authority}
    return None


def check_p1(row: dict, codegen: Verdict, protoc: Verdict,
             codegen_stderr: str = "") -> dict:
    match = codegen.accept == protoc.accept
    verdict = classify(row, "P1", True, match)
    result = {
        "property": "P1",
        "match": match,
        "codegen_accept": codegen.accept,
        "protoc_accept": protoc.accept,
        "classification": verdict,
        "root": row.get("root") if not match else None,
        "exception": row.get("exception") if not match else None,
    }
    if not match and not row.get("exception") and not codegen.accept and protoc.accept:
        policy = find_allowed_stricter(codegen_stderr)
        if policy:
            result["classification"] = "allowed-stricter"
            result["root"] = None
            result.update(policy)
    return result


def check_p2(
    row: dict,
    source: Verdict,
    descriptor: Verdict,
    source_header_abs: str | None,
    descriptor_header_abs: str | None,
) -> dict:
    applicable = source.accept
    if not applicable:
        return {
            "property": "P2",
            "applicable": False,
            "match": None,
            "classification": "n/a",
            "reason": "source path refused",
        }
    if not descriptor.accept:
        verdict = classify(row, "P2", True, False)
        return {
            "property": "P2",
            "applicable": True,
            "match": False,
            "reason": "descriptor path refused",
            "classification": verdict,
            "root": (row.get("p2_root") or row.get("root")),
            "exception": row.get("p2_exception"),
        }
    match = source.artifact.sha256 == descriptor.artifact.sha256
    verdict = classify(row, "P2", True, match)
    return {
        "property": "P2",
        "applicable": True,
        "match": match,
        "source_sha256": source.artifact.sha256,
        "descriptor_sha256": descriptor.artifact.sha256,
        "source_header": source.artifact.path,
        "descriptor_header": descriptor.artifact.path,
        "first_difference": first_differing_line(
            source_header_abs, descriptor_header_abs
        ),
        "classification": verdict,
        "root": (row.get("p2_root") or row.get("root")) if not match else None,
        "exception": row.get("p2_exception") if not match else None,
    }
