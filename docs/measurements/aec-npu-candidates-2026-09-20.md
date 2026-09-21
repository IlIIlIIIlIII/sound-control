# NPU AEC investigation — 2026-09-20

## Verified on this PC

- CPU: Intel Core Ultra 9 275HX.
- Intel AI Boost is present, device status OK; driver 32.0.100.4778.
- Isolated evaluation environment: `build/npu-env`, OpenVINO 2026.4.0.
- `Scripts/probe-windows-npu.py` compiles explicitly for NPU (no AUTO or CPU fallback), executes a synthetic matrix operation and checks the result.
- Device enumeration includes NPU; execution device reports NPU; numerical check passed. This establishes runtime availability, **not AEC quality or AEC real-time performance**.
- No neural AEC model has been installed into the active audio path.

## Latest research versus deployable models

| Candidate | Primary evidence | Missing requirement |
| --- | --- | --- |
| SpatialNet-Echo, ICASSP 2026 | [Official accepted paper](https://www.cmsworkshops.com/ICASSP2026/view_paper.php?PaperNum=3728) | No downloadable trained model or Intel NPU deployment located in this investigation; not benchmarked here. |
| LMPAN, July 2026 | [Paper](https://arxiv.org/abs/2607.02062): compact, joint AEC/NS architecture | No trained weights or NPU deployment located; research performance is not proof of local compatibility. |
| DiffVQE, May 2026 | [Paper](https://arxiv.org/abs/2605.08189) | No deployable streaming Intel NPU package located; cannot assume live latency from paper quality metrics. |
| EchoFree, August 2025 | [Author repository](https://github.com/StellanLi/EchoFree): 278K parameters, code/model release promised | Repository inspected still offers README rather than model weights. |
| LocalVQE | [Maintainer repository](https://github.com/localai-org/LocalVQE), [weights](https://huggingface.co/LocalAI-io/LocalVQE): Apache-2.0, streaming DeepVQE derivative | Published GGML/PyTorch implementation is not an Intel NPU package; no SOTA ranking established. Requires port and validation, not a ready replacement. |
| DTLN-AEC | [Author repository](https://github.com/breizhn/DTLN-aec): available pretrained models | Useful older baseline (2021), not a defensible latest-SOTA selection. |

## Commercial options

- [HANCE Intel announcement](https://hance.ai/blog/hance-and-intel-join-forces-in-a-strategic-collaboration) explicitly describes Intel NPU execution through an optional OpenVINO wrapper. However, noise removal and room dereverberation do not establish speaker-reference AEC.
- The former HANCE SDK repository now redirects to [Rapidly SDK](https://github.com/rapidly-labs/rapidly-sdk). Its public model table lists speech denoise, denoise/dereverb and music separation. The inspected public SDK does not demonstrate a reference-input AEC model or the advertised NPU wrapper. It requires a licence key. A vendor-provided **Windows x64 Intel NPU AEC evaluation package** would be needed before choosing it for this task; no purchase or vendor contact was made.
- [Krisp model guide](https://sdk-docs.krisp.ai/docs/rtc-model-guide-bvc-nc) documents noise and background-voice models, but does not establish this Intel NPU AEC deployment. Its background-voice removal may also conflict with preserving the iPad voice used as a test speaker.
- [Microsoft MAS model-based AEC](https://learn.microsoft.com/en-us/azure/ai-services/speech-service/audio-processing-model-based-echo-cancellation) documents microphone plus speaker-reference processing on Windows. The reviewed documentation does not provide an Intel NPU execution contract or an independent systemwide APO replacement.

Decision: no candidate currently satisfies all of **obtainable weights/SDK, actual Intel NPU execution, speaker-reference AEC, preserved near-end speech and proven long-running quality**. Do not label an untested candidate SOTA or silently deploy a CPU alternative.

## Live capture during research

The user enabled music and intermittent iPad speech. Captured 75 seconds locally; audio remains in ignored `build/aec-npu-evaluation-doubletalk.bin`, not uploaded. Diagnostic levels are in `aec-reference-restarted-2026-09-20.csv`.

At inspection, SoundControlWindows was absent and reference publication was frozen. Starting the installed app with `--startup` restored publication and timestamp progression. The capture includes both unavailable-reference and recovered-reference portions; it is **not** a clean model comparison. With concurrent speech, total input/output level change is not an echo-only ERLE metric.

This establishes one concrete failure mode (reference producer absent), not the cause of every previously reported long-running failure. The older per-instance frozen-model observations remain unresolved. No evidence here supports claiming the overall AEC issue fixed.
