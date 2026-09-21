# Residual echo burst mitigation

## Change

The learned AEC path could reopen all residual spectral gains when one 256-sample block failed the 5% energy-improvement check. Briefly bridge the first such block (5.33 ms at 48 kHz) only when the model is converged, far-end audio is active, input is not clipping, and estimated near-end spectral share is below 10%. Longer changes and likely near-end speech retain the previous bounded linear fallback. Reset the new consecutive-block counter with the canceller.

## Repeatable comparison

Captured 30 seconds of the user's music with simultaneous raw MOTU Loopback Mix (+6.0206 dB normalization), processed In 1-2, SMSL loopback, and MOTU output loopback. MOTU output loopback was silent. Replayed the exact timestamped raw microphone and speaker packets through the original and modified EchoStream. These are offline comparisons, not claims about universal listening quality.

After excluding the first 5 seconds, across 235 approximately 107 ms windows:

| Metric | Original | Modified |
|---|---:|---:|
| Windows with less than 10 dB attenuation | 17 | 11 |
| 10th-percentile attenuation | 11.20 dB | 12.96 dB |

This is a 35% reduction in poorly suppressed windows on this recording. Residual bursts remain.

## Voice protection

Captured the iPad alone with Spotify paused, verified SMSL reference at the silence floor, then mixed that known voice into the same music-only microphone packets after ten seconds. Projected output onto the known voice and echo sources, accounting for EchoStream's 1024-frame latency:

| Voice amplitude | Original voice gain | Modified voice gain | Original echo attenuation | Modified echo attenuation |
|---|---:|---:|---:|---:|
| Actual capture level | -3.792 dB | -3.897 dB | 17.333 dB | 18.075 dB |
| 0.3 × capture level | -12.461 dB | -12.987 dB | 28.251 dB | 29.236 dB |
| 3 × capture level | -0.751 dB | -0.753 dB | 8.437 dB | 8.576 dB |

Quiet voice preservation is still a known weakness; the update does not solve it. More aggressive candidates were rejected because they attenuated voice substantially. No subjective listening score was assigned.

## Validation and installation

All five native CTest suites passed, including double-talk, quiet-passage voice, near-source bursts, streaming, APO and bridge checks. Existing tests exercise the relevant voice constraints; the private recorded fixtures stay in ignored `build/` rather than being committed.

Installed APO SHA256: `521931BEB90611900048FB47ECF7D23144EC51E385DF04EA6C97F158BD0447DE`.

Previous APO backup: `build/aec-before-burst-apo.dll`. Only the APO DLL was replaced; audio service was restarted. UI and installed bridge were retained. AEC remains enabled.

Following installation, the audio service was running and the installed file hash matched the tested artifact. Spotify playback was reconnected after the service restart. The final live probe confirmed active processing, non-silent SMSL reference, and no reference underruns in the observed run. iPad speech remained enabled, so live total RMS attenuation is not a pure echo-rejection score. The exact before/after burst count above comes from replaying the identical music-only fixture.
