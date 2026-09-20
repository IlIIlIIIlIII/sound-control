#!/bin/zsh
set -euo pipefail

if [[ "${1:-}" != "--confirm-system-change" ]]; then
  echo "Usage: $0 --confirm-system-change"
  echo "Stops SoundControl and removes its app, microphone driver, old driver, and LaunchAgent. Settings are preserved."
  exit 2
fi

launch_agent="/Users/sunggu/Library/LaunchAgents/io.griplabs.soundcontrol.engine.plist"
launchctl bootout gui/"$(id -u)" "${launch_agent}" 2>/dev/null || true
rm -rf /Users/sunggu/Applications/SoundControl.app
if [[ -d /Library/Audio/Plug-Ins/HAL/SoundControl.driver || \
      -d /Library/Audio/Plug-Ins/HAL/SoundControlMic.driver ]]; then
  sudo rm -rf /Library/Audio/Plug-Ins/HAL/SoundControl.driver \
    /Library/Audio/Plug-Ins/HAL/SoundControlMic.driver
  sudo killall coreaudiod
fi
rm -f /tmp/io.griplabs.soundcontrol.mic.shared
rm -f "${launch_agent}"
echo "SoundControl binaries removed. Settings remain in ~/Library/Application Support/SoundControl."
