#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Specialize the shipped arrhythmia batch signatures without repacking its graph."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import numpy as np
from ai_edge_litert import schema_py_generated as schema
from ai_edge_litert.interpreter import Interpreter, OpResolverType

ROOT = Path(__file__).resolve().parents[2]
ORIGINAL_SHA256 = "a1855af0e0b2a346d773e0d36ff8c8b986c51db4aed99a6066c9da9f23f12eab"
BATCH_ONE_SHA256 = "8b91d202ac5b94201277daf1ad3dc8b381bdceb99a1ca7261f8eaafb0f7cc7a0"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def interpreter(data):
    runner = Interpreter(model_content=data, num_threads=1,
                         experimental_op_resolver_type=OpResolverType.BUILTIN_REF,
                         experimental_preserve_all_tensors=True)
    runner.resize_tensor_input(runner.get_input_details()[0]["index"], [1, 500, 1], strict=True)
    runner.allocate_tensors()
    return runner


def specialize(original):
    require(digest(original) == ORIGINAL_SHA256, "unexpected source model")
    runner = interpreter(original)
    runtime = {item["index"]: item for item in runner.get_tensor_details()}
    graph = schema.Model.GetRootAsModel(original, 0).Subgraphs(0)
    patched = bytearray(original)
    changes = []
    for index in range(graph.TensorsLength()):
        tensor = graph.Tensors(index)
        signature = tensor.ShapeSignatureAsNumpy()
        if not isinstance(signature, np.ndarray) or not np.any(signature < 0):
            continue
        shape = tensor.ShapeAsNumpy()
        require(np.array_equal(shape, runtime[index]["shape"]), "concrete/runtime shape differs")
        require(signature[0] == -1 and shape[0] == 1 and
                np.array_equal(signature[1:], shape[1:]), "non-batch dynamic dimension")
        # The schema accessor exposes a view into the original vector. Obtain
        # its byte offset rather than serializing or reconstructing the graph.
        offset = signature.__array_interface__["data"][0] - np.frombuffer(original, dtype=np.uint8).__array_interface__["data"][0]
        require(0 <= offset <= len(original) - 4 and struct.unpack_from("<i", original, offset)[0] == -1,
                "signature vector offset mismatch")
        struct.pack_into("<i", patched, offset, 1)
        changes.append({"tensor": index, "field": "shape_signature[0]", "offset": offset,
                        "before": -1, "after": 1, "shape": shape.tolist()})
    require(len(changes) == 48 and digest(patched) == BATCH_ONE_SHA256, "unexpected specialization")
    candidate = interpreter(bytes(patched))
    for method in ("get_input_details", "get_output_details"):
        a, b = getattr(runner, method)(), getattr(candidate, method)()
        require(len(a) == len(b) == 1, "IO count mismatch")
        require(a[0]["dtype"] == b[0]["dtype"] and np.array_equal(a[0]["shape"], b[0]["shape"]), "IO contract mismatch")
    cases = []
    for case in range(8):
        name = "golden-arr.npz" if case == 0 else f"golden-arr_case{case:02}.npz"
        with np.load(ROOT / "tools/aot/golden" / name, allow_pickle=False) as fixture:
            stimulus = fixture["input_0"]
        outputs = []
        for model in (runner, candidate):
            model.set_tensor(model.get_input_details()[0]["index"], stimulus)
            model.invoke()
            outputs.append(model.get_tensor(model.get_output_details()[0]["index"]).tobytes())
        require(len(outputs[0]) == 16 and outputs[0] == outputs[1], f"case {case} full output differs")
        cases.append({"case": case, "input_sha256": digest(stimulus.tobytes()),
                      "output_sha256": digest(outputs[0]), "bit_exact": True})
    return bytes(patched), {"source_sha256": ORIGINAL_SHA256, "batch_one_sha256": BATCH_ONE_SHA256,
                            "changes": changes, "cases": cases}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "tools/aot/arrhythmia-batch1.tflite")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    data, evidence = specialize((ROOT / "assets/arrhythmia.tflite").read_bytes())
    receipt = args.output.with_suffix(".json")
    text = json.dumps(evidence, indent=2) + "\n"
    if args.check:
        require(args.output.read_bytes() == data and receipt.read_text() == text, "specialization drift")
    else:
        args.output.write_bytes(data)
        receipt.write_text(text)
    print("Batch-one specialization and eight full-output equivalence cases PASS")


if __name__ == "__main__":
    main()
