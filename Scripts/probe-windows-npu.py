"""Verify real OpenVINO NPU execution; this is not an AEC quality benchmark.

Run in an isolated environment with openvino and numpy installed.
Never use AUTO: CPU fallback would invalidate the NPU requirement.
"""
import json

import numpy as np
import openvino as ov
from openvino import opset13 as op


def main():
    core = ov.Core()
    sample = op.parameter([1, 256], np.float32)
    weights = op.constant(np.eye(256, dtype=np.float32) * 0.5)
    model = ov.Model([op.matmul(sample, weights, False, False)], [sample])
    compiled = core.compile_model(model, "NPU")
    result = next(iter(compiled([np.ones((1, 256), np.float32)]).values()))
    correct = bool(np.allclose(result, 0.5))
    print(json.dumps({
        "openvino": ov.__version__,
        "available_devices": core.available_devices,
        "npu_name": core.get_property("NPU", "FULL_DEVICE_NAME"),
        "execution_devices": compiled.get_property("EXECUTION_DEVICES"),
        "synthetic_result_correct": correct,
        "aec_model_tested": False,
    }, indent=2))
    if not correct:
        raise RuntimeError("NPU returned an incorrect synthetic result")


if __name__ == "__main__":
    main()
