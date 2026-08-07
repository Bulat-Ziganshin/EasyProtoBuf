#!/usr/bin/env python3
"""Compare EasyProtoBuf parser output with FileDescriptorSet from protoc."""

import argparse
import pathlib
import subprocess
import sys
import tempfile

CASES = [
    ("descriptor.proto", None),
    ("onnx.proto3", None),
    ("caffe.proto", None),
    ("PulsarApi.proto", None),
    ("mesos.proto", None),
    ("envoy-route-components.proto", "envoy"),
    ("kubernetes-core-v1-generated.proto", "kubernetes"),
]


def run(command):
    print("+", " ".join(str(x) for x in command))
    subprocess.run([str(x) for x in command], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--protoc", required=True, help="path to official protoc executable")
    parser.add_argument("--compare", required=True,
                        help="path to parser-differential-compare executable")
    parser.add_argument("--case", action="append", dest="cases",
                        help="run only the named corpus file; may be repeated")
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parent
    corpus = root / "corpus"
    selected = set(args.cases or [])

    with tempfile.TemporaryDirectory(prefix="easypb-differential-") as temporary:
        output = pathlib.Path(temporary)
        for filename, stub_group in CASES:
            if selected and filename not in selected:
                continue
            source = corpus / filename
            descriptor = output / (filename + ".pb")
            command = [args.protoc, "-I", corpus]
            if stub_group:
                command += ["-I", root / "stubs" / stub_group]
            command += ["--descriptor_set_out", descriptor, source]
            run(command)
            run([args.compare, source, descriptor])

    print("all differential parser tests passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
