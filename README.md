# MacTools

MacTools is a small, native macOS utility. It loads separate REW
`Configurable_PEQ` text files for the left and right channels and routes the
system mix to a user-selected physical output device. It also removes that
speaker signal from MOTU M2 input 1 before presenting it to voice apps as a
standard one-channel `MacTools Mic` device, and keeps
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
- Short second/third-order Hammerstein paths in the stronger profiles for
  loudspeaker distortion without a neural model or an extra audio buffer
- One-channel, input-only Audio Server Driver Plug-in for Discord/WebRTC
- Validated EDID maintenance for the two 32RTX950 displays
- Global `⌃⌥⌘R` external-display reinitialization shortcut with crash recovery;
  it reconnects the two MO32U24 displays first, then the dock-connected
  32RTX950, and the direct-HDMI 32RTX950 last
- Global `⌃⌥⌘S` shortcut that swaps the position, rotation, and main-display
  roles of exactly two MO32U24 displays
- No third-party runtime libraries, network access, telemetry, or updater

Display reinitialization and virtual EDID use dynamically loaded, undocumented
macOS APIs. MacTools checks that the required symbols are present and otherwise
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

The resulting app is `build/MacTools.app`. Building and testing do not install
anything or change the current output.

## Local installation

Installation places `MacToolsMic.driver` in the system HAL plug-in directory and
restarts Core Audio once:

```sh
./Scripts/install-local.sh --confirm-system-change
```

Open MacTools, choose a physical output device, import an L file and an R file,
then enable EQ. Imported files are copied to
`~/Library/Application Support/MacTools/Filters`. An existing MacSound settings
directory is copied once when MacTools is first launched or installed.

`스피커 소리 제거` is independent from EQ and defaults to `최고 음질`.

The always-running engine also owns a lightweight menu-bar item. It exposes the
selected physical output, EQ and echo-cancellation controls, both display
actions, engine reload, and a link to the full settings window without adding a
second resident process.
The `균형` and `강한 제거` profiles trade progressively stronger residual echo
suppression for more processing during simultaneous speech. Only the live
speaker reference is processed; MacTools does not save or transmit audio.
`최고 음질` prioritizes near-end voice, `균형` adds a third-order loudspeaker
path, and `강한 제거` adds second- and third-order paths. If M2 input 1 reaches
approximately -0.5 dBFS, adaptation freezes and the menu reports an input
overload because clipped microphone samples cannot be reconstructed.

On first enable, allow **MacTools Engine** under System Settings > Privacy &
Security > System Audio Recording. The selected physical device remains the
normal macOS output; bypass or an engine exit immediately restores its original
direct audio path.

When an enabled USB target is disconnected, MacTools stops its tap, clears the
active EQ checkbox, and waits without muting the fallback device. When the same
target returns, the EQ route and checkbox are restored and that device is
automatically selected as both the normal and system default output.

The local install script places the control app in `~/Applications/MacTools.app`.
When M2 is connected, the engine captures only hardware input channel 1 and sets
`MacTools Mic` as the normal macOS input. On disconnect the virtual microphone is
removed from the Core Audio device list and the default input falls back to the
built-in microphone (or another physical input). On reconnect it is published
again and becomes the default input automatically. Loopback is neither required
nor modified.

Uninstalling preserves imported filters:

```sh
./Scripts/uninstall-local.sh --confirm-system-change
```

## Supported REW rows

MacTools accepts enabled peaking filters in this shape:

```text
Notes:left
Configurable_PEQ
Number Enabled Control Type Frequency(Hz) Gain(dB) Q
1 True Auto PK 66.00 -12.00 7.40
```

Only enabled `PK` rows are applied. `None` and disabled rows are ignored.
An invalid import never replaces the last valid channel file.
