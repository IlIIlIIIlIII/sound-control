# Neural echo leakage after near speech — 2026-09-20

The user reported intermittent speaker leakage after speaking. The previous continuity test established that callbacks and reference delivery continued, not that residual echo was inaudible.

## Reproduction and changes

Captured 30 seconds after the user explicitly confirmed PC music only and iPad speech stopped. Raw MOTU Loopback Mix input 1 is normalized by +6.0206 dB; its output loopback remained silent. Speaker reference is the SMSL WASAPI loopback. Independently recorded near speech is digitally mixed into seconds 10–18. The silent-near tail is measured at seconds 19–29. This is a controlled replay, not an acoustic double-talk ground truth or perceptual score. Earlier stored music-only data had peaks poorly correlated with the reference; those peaks alone cannot establish speaker leakage.

Two changes:

- DTLN-AEC 256 recurrent state relaxation: multiply both stages' carried states by 0.995 only when model output energy is below 10% of the corresponding microphone hop and the reference has energy. The microphone comparison uses the oldest 128 samples in the 512-sample model window, matching output delay. No output gate or overlap reset. State relaxation is an empirically validated modification to the pretrained inference behavior, not a new model or a general SOTA claim. Inference remains explicitly on Intel NPU.
- Correct missing-result crossfade: retain the last wet sample during the 240-frame transition. Previously substituting dry as the missing wet value made the advertised ramp instantly output raw microphone audio. This is a separately verified fault; the observed steady-reference leakage did not coincide with increasing missing counters.

## Measurements

48 kHz paced C++ `NeuralStream` replay, including actual input/output FIR conversion, NPU worker scheduling and 4096-frame delay, using the same fixture:

| Measure | Before | After |
|---|---:|---:|
| Post-voice tail RMS, dBFS | -50.37 | -54.10 |
| Post-voice 95th-percentile 100 ms RMS, dBFS | -45.11 | -47.99 |
| Weakest 100 ms tail echo reduction, dB | 7.09 | 12.51 |
| Near-voice projection gain, dB | -1.31 | -1.21 |
| Near-voice SI-SDR, dB | 9.99 | 10.06 |
| Missing model output frames | 0 | 0 |

See `aec-neural-tail-stream-2026-09-20.json`. Tail energy decreases 3.72 dB; weakest-window removal improves 5.42 dB. The same 85.3 ms fixed latency and 16 kHz model bandwidth remain.

Native 16 kHz model replay repeated the 30-second fixture six times **without resetting state** (180 seconds of audio). Baseline worst-window ERLE settles around 7.38 dB; modified stays around 11.72 dB. Modified tail RMS stays -52.55 dBFS across all cycles. Normal near speech projection was -1.22 to -1.32 dB before and -1.40 dB after; SI-SDR about 10.0 before and 9.45 after. These differ slightly from the paced 48 kHz path due to resampling. See `aec-neural-tail-recovery-2026-09-20.json`.

Quiet voice is still a limitation. On this fixture, the Python NPU replay's quiet-voice gain improved from -16.43 to -8.52 dB, but that remains substantial attenuation. On the older independent full-voice regression fixture, normal gain stayed about -2.3 dB and SI-SDR 7.29→7.26 dB. Quiet-voice SI-SDR slightly regressed (-1.49→-1.90 dB); this change is not uniformly better on every measure.

## Validation and deployment

- Six CTest suites passed, including missing-result/recovery transition regression and unavailable-model delayed passthrough.
- `Tests/WindowsNeuralReplay.cpp` / `PersonalToolsNeuralReplay` provide a manual local-weight NPU runner; `Scripts/score-windows-neural-tail.py` scores controlled repeated replay. Private raw audio remains under ignored `build/`.
- Built and installed APO SHA256: `D62ED6AABE3911D1DB8CB03353834C82BC876D4F2172AE8FFAE8E669414A757F`.
- Prior installed NPU APO preserved in `build/before-neural-tail-update/PersonalToolsAPO.dll`.
- One elevated replacement of the existing APO, with audio-service restart. No model weights, signing/protection settings, microphone selection, or EQ configuration changed. Packaged APO updated too.
- PersonalTools restarted as the normal user. Spotify playback had to be resumed after service restart; while paused, reference absence correctly caused delayed raw fallback. Subsequent sampled NPU blocks advanced with reference active and no new missing frames.
- A 75-second installed WASAPI capture completed; `aec-neural-tail-installed-75s-2026-09-20.csv` contains 74 one-second rows. After startup, the missing counter remained 3902 and reference stayed active. UI independently showed DTLN-AEC 256 / NPU processing and active speaker reference. User confirmation of the requested post-installation short utterance did not arrive during this capture, so it is continuity evidence, not a verified live voice-on/off A/B comparison.
- The existing sound-settings subtitle still displays the old DSP latency (21.33 ms); actual NPU latency is 85.33 ms. UI was not rebuilt in this APO-only update.

This mitigates measured post-voice leakage; it does not establish complete inaudibility in all rooms, songs, voice levels, or long-running sessions.
