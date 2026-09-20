# sound-control

Windows 11용 시스템 EQ·실제 마이크 반향 제거 APO와 네이티브 설정 앱도 포함합니다.
Windows 빌드, 로컬 설치, 서명 요구 사항과 검증 범위는 [Windows 안내](docs/windows.md)를 참고하세요.
가상 마이크와 디스플레이 기능은 Windows 빌드에 포함하지 않습니다.

아래는 기존 macOS 버전 설명입니다.

SoundControl is a small, native macOS utility. It loads separate REW
`Configurable_PEQ` text files for the left and right channels and routes the
system mix to a user-selected physical output device. It also removes that
speaker signal from MOTU M2 input 1 before presenting it to voice apps as a
standard one-channel `SoundControl Mic` device, and keeps
the local external-display configuration available without BetterDisplay.

## Design

- Core Audio Process Tap scoped to the selected physical output device
- Objective-C++ engine using a process-private aggregate device; no visible
  virtual output device is installed
- Accelerate/vDSP cascaded biquads with independent L/R state
- Native AppKit control window with no WebView or visualization
- Direct HAL capture of M2 input 1 at 48 kHz into a lock-free shared-memory ring
- Mid/Side-conditioned, partitioned frequency-domain acoustic echo cancellation
  using the post-EQ speaker signal, host-time ASRC, and band-aware double-talk
  protection
- Stable foreground and on-demand shadow filters for automatic speaker-path
  recovery, plus a 257-bin residual echo suppressor for correlated music
- Automatically accepted second/third-order Hammerstein paths for loudspeaker
  distortion without a neural model or an extra audio buffer
- One-channel, input-only Audio Server Driver Plug-in for Discord/WebRTC
- Validated EDID maintenance for the two 32RTX950 displays
- Global `⌃⌥⌘R` external-display reinitialization shortcut with crash recovery;
  it reconnects the two MO32U24 displays first, then the dock-connected
  32RTX950, and the direct-HDMI 32RTX950 last
- Global `⌃⌥⌘S` shortcut that swaps the position, rotation, and main-display
  roles of exactly two MO32U24 displays
- No third-party runtime libraries, network access, telemetry, or updater

Display reinitialization and virtual EDID use dynamically loaded, undocumented
macOS APIs. SoundControl checks that the required symbols are present and otherwise
leaves the display configuration unchanged. The role swap uses the public
CoreGraphics display-configuration transaction API for positions and dynamically
loads the same private MonitorPanel rotation path used by macOS display settings.

The build pins and downloads Apple's official NullAudio AudioServerPlugIn sample
by SHA-256, then compiles a narrowed input-only driver around its required HAL
property plumbing. The installed app and driver do not access the network.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The resulting app is `build/SoundControl.app`. Building and testing do not install
anything or change the current output.

## Local installation

Installation places `SoundControlMic.driver` in the system HAL plug-in directory and
restarts Core Audio once:

```sh
./Scripts/install-local.sh --confirm-system-change
```

Open SoundControl, choose a physical output device, import an L file and an R file,
then enable EQ. Imported files are copied to
`~/Library/Application Support/SoundControl/Filters`.

`스피커 소리 제거` is independent from EQ and has one automatic mode.

The always-running engine also owns a lightweight menu-bar item. It exposes the
selected physical output, EQ and echo-cancellation controls, both display
actions, engine reload, and a link to the full settings window without adding a
second resident process.
The automatic mode protects frequency bands containing near-end speech while
applying stronger residual and nonlinear echo removal only to render-correlated
bands. Only the live speaker reference is processed; SoundControl does not save or
transmit audio. If M2 input 1 reaches approximately -0.5 dBFS, adaptation
freezes and the menu reports an input overload because clipped microphone
samples cannot be reconstructed.

The established echo filter stays active during sudden near-end speech bursts,
with the same per-band voice protection. An independent nearby device is treated
as near-end audio: only sound correlated with the selected speaker reference
is targeted. No additional microphone buffer or lookahead is used.

During quiet passages, a converged, bounded model can continue linear echo
subtraction even when its improvement is too small for the residual suppressor's
confidence gate. This path avoids suppressing nearby speech and continuously
limits subtraction so each block's energy can increase by at most 10%.
This fallback is disabled for unlearned, non-finite, unbounded, or clipped cases.
Runtime status separately counts model bypass and linear-only blocks,
since total microphone reduction alone does not identify a raw-audio bypass.

An independent delay monitor checks up to 800 ms of the speaker-to-microphone
path. It warns at 200 ms and requests a capture/rate-cycle recovery after three
consistent, confident measurements at or above the canceller's 256 ms span.
Silence, ambiguous periodic audio, missing reference, heavily clipped input, and
unrelated near-end speech are not sufficient evidence for a reset. Isolated
overloaded points are excluded; at least 95% of the observation must remain usable.
The delay includes the
whole render/acoustic/capture path; it is not a measurement of the M2 driver alone.
The control-thread recovery stops capture, cycles 48 kHz to 44.1 kHz and back,
checks the restored rate, then restarts capture. A microphone interruption may
occur. Failures and timeouts attempt to restore 48 kHz, and a restart is reported
as verified only after two healthy delay measurements. Without suitable speaker
audio, verification remains pending and then warns after 30 seconds.

Recovery attempts are limited to one per two minutes and three per fifteen
minutes within the current engine run. The menu's `마이크 경고 · 복구 기록` submenu
and the settings window show current warnings and timestamped recovery events.
The latest 64 events survive engine restarts in
`~/Library/Application Support/SoundControl/mic-health.json`; audio is not saved.
Delay analysis runs off the audio callback using a bounded queue.

On first enable, allow **SoundControl Engine** under System Settings > Privacy &
Security > System Audio Recording. The selected physical device remains the
normal macOS output; bypass or an engine exit immediately restores its original
direct audio path.

When an enabled USB target is disconnected, SoundControl stops its tap, clears the
active EQ checkbox, and waits without muting the fallback device. When the same
target returns, the EQ route and checkbox are restored and that device is
automatically selected as both the normal and system default output.

The local install script places the control app in `~/Applications/SoundControl.app`.
When M2 is connected, the engine captures only hardware input channel 1 and sets
`SoundControl Mic` as the normal macOS input. On disconnect the virtual microphone is
removed from the Core Audio device list and the default input falls back to the
built-in microphone (or another physical input). On reconnect it is published
again and becomes the default input automatically. Loopback is neither required
nor modified.

Uninstalling preserves imported filters:

```sh
./Scripts/uninstall-local.sh --confirm-system-change
```

## Supported REW rows

SoundControl accepts enabled peaking filters in this shape:

```text
Notes:left
Configurable_PEQ
Number Enabled Control Type Frequency(Hz) Gain(dB) Q
1 True Auto PK 66.00 -12.00 7.40
```

Only enabled `PK` rows are applied. `None` and disabled rows are ignored.
An invalid import never replaces the last valid channel file.
