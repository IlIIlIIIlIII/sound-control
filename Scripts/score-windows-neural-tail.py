"""Score local NPU replay output against an independently mixed voice fixture.

Usage: python score-windows-neural-tail.py fixture.npz before.f32 after.f32 report.json
The fixture contains 48 kHz echo/near/ref; near speech must be confined to 10–18 s.
Replay input is 16 kHz interleaved echo+near/reference, truncated to 128-frame hops.
Outputs can contain repeated fixture cycles with continuously retained model state.
Audio stays local. These are signal comparisons, not perceptual or live-room scores.
"""
import json
import sys
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly


def score(output, echo, near):
    tail = slice(19 * 16000, 29 * 16000)
    speech = slice(12 * 16000, 18 * 16000)
    y, e = output[tail].reshape(-1, 1600), echo[tail].reshape(-1, 1600)
    power_y = np.mean(y.astype(float) ** 2, axis=1)
    power_e = np.mean(e.astype(float) ** 2, axis=1)
    gain = np.linalg.lstsq(np.stack([near[speech], echo[speech]], axis=1), output[speech], rcond=None)[0][0]
    target = gain * near[speech]
    return {
        "tail_rms_dbfs": float(10 * np.log10(np.mean(power_y) + 1e-15)),
        "tail_p95_100ms_dbfs": float(10 * np.log10(np.percentile(power_y, 95) + 1e-15)),
        "tail_worst_100ms_erle_db": float(np.min(10 * np.log10((power_e + 1e-15) / (power_y + 1e-15)))),
        "voice_gain_db": float(20 * np.log10(abs(gain) + 1e-15)),
        "voice_si_sdr_db": float(10 * np.log10((np.sum(target ** 2) + 1e-15) / (np.sum((output[speech] - target) ** 2) + 1e-15))),
    }


if __name__ == "__main__":
    fixture, before, after, destination = map(Path, sys.argv[1:])
    data = np.load(fixture)
    echo, near = [resample_poly(data[k], 1, 3).astype(np.float32) for k in ["echo", "near"]]
    cycle_frames = len(echo) // 128 * 128
    echo, near = echo[:cycle_frames], near[:cycle_frames]
    if cycle_frames < 29 * 16000 or np.any(data["near"][18 * 48000:]):
        raise ValueError("Expected at least 29 seconds and voice ending at 18 seconds")
    result = {"sample_rate": 16000, "model_delay_removed_samples": 384, "cycles": {}}
    for name, path in [("before", before), ("after", after)]:
        output = np.fromfile(path, dtype=np.float32)[384:]
        result["cycles"][name] = [score(output[start:start + cycle_frames], echo, near)
                                   for start in range(0, len(output) - 29 * 16000 + 1, cycle_frames)]
    Path(destination).write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
