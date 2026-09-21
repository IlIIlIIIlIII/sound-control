# Live toggle comparison

User reported no audible difference between on and off. User confirmed music alone playing from SMSL, without iPad speech.

The initial configuration was EQ on, AEC off. Captured simultaneous MOTU Loopback Mix, processed In 1-2, SMSL loopback and MOTU output loopback. The raw proxy is normalized by +6.0206 dB; this is the same setup verified earlier. MOTU output remained at the numerical silence floor. Excluded the first three seconds and used per-second input/output RMS differences; this is not a speech intelligibility score or a direct measurement of the Recorder file.

- First pass off: median 0 dB, range -0.28 to 1.08 dB (24 music-active rows).
- First pass on: median 15.35 dB, range 4.27 to 23.77 dB (26 rows).
- Repeated with Windows Recorder actively recording In 1-2: enabled median 17.15 dB, range 2.22 to 33.22 dB (26 rows).
- Windows Recorder saved `녹음 (2)` of approximately 84 seconds. AEC was off around 12–43 seconds and on from around 43 seconds onward. Computer Use screenshots showed sustained larger waveforms in the off interval and smaller waveforms with intermittent bursts after enabling. No playback listening or decoding of the saved file was performed.

AEC left enabled. No binaries or audio processing code changed. Toggle-off/on changes were the only audio configuration changes. Results demonstrate attenuation in the current setup but inconsistent residual suppression; they do not prove the earlier user recording was processed correctly or that echo is fully removed.
