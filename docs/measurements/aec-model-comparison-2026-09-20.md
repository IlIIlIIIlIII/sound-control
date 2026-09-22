# Local AEC model comparison and NPU replacement

## Selection

DTLN-AEC 256 is the best balanced tested candidate on this PC's saved recordings. It is an older model, not a claim of current research SOTA. LocalVQE 1.3 removed more echo-only energy but attenuated the wanted voice too much. DTLN 128 removed more music-only energy than 256, but 256 preserved speech better across the three voice levels.

## Method

- Same 30-second music-only recording, raw MOTU Mix input normalized by the independently checked factor 2.
- Separate iPad speech recording taken after the music had stopped, mixed into the music microphone signal at 10 seconds. This is a reproducible digital mixture of real recordings, not a controlled simultaneous acoustic ground truth.
- Voice scale 0.3, 1 and 3. Score from 14 seconds onward; no clipping or loudness normalization before scoring.
- All outputs scored at 16 kHz to compare the neural models fairly. Existing C++ output is delayed by 1024 samples at 48 kHz; DTLN output is delayed by 384 samples at 16 kHz. These fixed delays are removed before comparison. Failing to correct DTLN delay produces invalid speech-loss measurements.
- Speech gain and echo projection use least squares against known separately recorded speech and echo. SI-SDR is relative to the known speech component. These are diagnostics, not listener MOS. Independent voice recording includes its room/background noise.
- DTLN neural inference explicitly targets `NPU`; there is no automatic CPU/GPU fallback. LocalVQE 1.3 was screened on CPU only and rejected for quality before NPU porting.

## Results

| Model | Music-only energy reduction (dB, higher better) | Normal speech gain (dB, 0 best) | Normal speech SI-SDR (dB, higher better) | Quiet speech gain (dB, 0 best) |
| --- | ---: | ---: | ---: | ---: |
| Existing adaptive DSP | 20.21 | -3.91 | 1.65 | -12.97 |
| DTLN-AEC 128, NPU | 28.04 | -2.89 | 5.84 | -9.90 |
| **DTLN-AEC 256, NPU** | **23.46** | **-2.32** | **7.29** | **-8.57** |
| DTLN-AEC 512, NPU | 8.95 | -17.25 | -6.96 | -21.56 |
| LocalVQE 1.3, CPU | 62.16 | -14.15 | 0.54 | -26.25 |

The 512 result describes this tested deployment/fixture only, not every backend or environment. Quiet speech remains imperfect even in the selected model.

DTLN 256's NPU inference loop (FFT, two model invocations, recurrent state copy and overlap-add) takes about 1.3 ms median per 8 ms hop, about 1.55 ms p99 in the measured run. CPU comparison gives normal-speech gain -2.33 dB and SI-SDR 7.28 dB, close to NPU's -2.32/7.29, checking that this model's NPU execution has not introduced a material quality regression.

The native C++ NPU runner matches the Python NPU reference to about 0.33% relative RMS after warmup. The actual streaming C++ path with causal sample-rate conversion scores normal-speech gain -2.46 dB, echo projection reduction 20.19 dB and SI-SDR 6.87 dB. A paced 30-second 48-kHz streaming run processed 1,440,000 frames with zero missing neural results after startup latency. A shorter 64-ms buffering attempt missed deadlines and was not selected.

## Runtime integration

- `DtlnNpu.hpp`: dynamic OpenVINO C API, explicit NPU compilation, fixed recurrent states and 512-point FFT.
- `NeuralStream.hpp`: per-APO worker and bounded SPSC queues; no model loading, inference, waits or heap allocation in the audio callback.
- Fixed 4096-frame latency at 48 kHz: 85.33 ms, including model and worker scheduling. APO reports this latency consistently, including bypass.
- Mono microphone input 1; neural processing at 16 kHz. This limits the processed voice bandwidth to roughly 8 kHz.
- If the model fails or misses a deadline, output uses the latency-matched original microphone rather than silence. This preserves speech but can let echo through; it is not a successful AEC result.
- Opt-in deployment flag: `npu/enabled.flag` next to the installed APO. Runtime and weights also reside there. Without the flag, the existing implementation remains active.
- App status distinguishes NPU initialization, processing, reference absence and initialization failure. PersonalTools must remain in the tray to provide the selected speaker's reference signal.
- Six CTest suites pass, including disabled/failed-neural-path channel and latency preservation. Live installed validation is separate and must be recorded before claiming long-running stability.

## Provenance

- [DTLN-AEC author repository](https://github.com/breizhn/DTLN-aec), revision `9d24e128b4f409db18227b8babb343016625921f`, MIT licence; attribution included in deployment.
- `dtln_aec_256_1.tflite` SHA256 `4a3a588b69fd79d837bc068b579a26faa92cac39dddbb00001d2dc1c3d869d60`.
- `dtln_aec_256_2.tflite` SHA256 `fa2590243aad1bf893c5be45b20709e8c50feec65e3604d1d52bae6eeddc23d3`.
- [LocalVQE maintainer repository](https://github.com/localai-org/LocalVQE), revision `f53063c9eb2a85f96479867d1dd911dc3bf6319b`.
- `localvqe-v1.3-4.8M.pt` SHA256 `22d3e2f33bb8b25ec1c6a928cfb741bb631d45bae2b3759684818b101c95878e`, verified before `torch.load(weights_only=True)`.
- OpenVINO 2026.4.0; Intel AI Boost, driver 32.0.100.4778; Core Ultra 9 275HX.

Private captured audio and generated comparison WAVs remain in ignored `build/` and were not uploaded.

## Reproduction and packaging

The local evaluation scripts are `Scripts/compare-windows-aec.py` (LocalVQE and shared metrics) and `Scripts/compare-windows-dtln.py` (explicit NPU inference for DTLN sizes 128/256/512). They expect the private packet fixtures, model checkouts and checkpoint under `build/`, as above. The Python environment requires NumPy, SciPy, OpenVINO and, for LocalVQE screening only, CPU PyTorch/einops. These tools do not record live audio or upload files.

`Scripts/package-windows-npu.ps1` verifies both selected model hashes and packages runtime DLLs, licences, model files and the opt-in flag into an existing Windows package. The regular Windows installer includes this optional folder when present. Models/runtime binaries are intentionally not committed to the repository.

Rollback: keep the prior APO/bridge copies in `build/before-npu-upgrade/`. Removing the installed `npu/enabled.flag` and restarting Windows audio activates the original implementation on newly created APO instances; this requires administrator access. Restore the backed-up DLLs as well to revert the implementation completely.

## Installed validation

Installed APO SHA256: `2f0992cad766b7e71017390eb2e2cf8f8b9b9aea5c840c745996049b95919b7a`. Both installed model hashes match the pinned originals. Existing speaker EQ remains enabled. Startup registration still launches the installed app with `--startup`.

The first hardware run exposed occasional late loopback packets despite successful standalone timing tests. The final worker retries missing reference within a bounded 30-ms arrival window before using the dry fallback. After this correction, a continuous 180-second WASAPI capture completed successfully. Following initialization, the missing-output counter stayed at 3584 (no additional missing frames); NPU completed-block counters advanced, state stayed ready, and error stayed zero in sampled diagnostics. The app's processing page visibly reports `DTLN-AEC 256 · NPU 처리 중`.

The CSV is `aec-npu-installed-180s.csv`. This establishes observed three-minute continuity, not indefinite stability. The user had enabled music and intermittent iPad speech; a later request to confirm music-only conditions was unanswered during this run. Therefore live combined-signal RMS ratios are not presented as echo-only cancellation scores. Voice preservation figures above come from the controlled saved-mixture comparison.

Validation: six CTest suites passed, Windows installer rollback tests passed, packaging hashes checked, Python scripts syntax-checked, and the installed app was inspected with the Computer Use skill. No new registry audio-protection change was needed for this upgrade.
