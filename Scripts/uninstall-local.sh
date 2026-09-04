#!/bin/zsh
set -euo pipefail

if [[ "${1:-}" != "--confirm-system-change" ]]; then
  echo "Usage: $0 --confirm-system-change"
  echo "Stops MacTools and removes its app, microphone driver, old driver, and LaunchAgent. Settings are preserved."
  exit 2
fi

launch_agent="/Users/sunggu/Library/LaunchAgents/io.griplabs.macsound.engine.plist"
launchctl bootout gui/"$(id -u)" "${launch_agent}" 2>/dev/null || true
rm -rf /Users/sunggu/Applications/MacTools.app /Users/sunggu/Applications/MacSound.app
if [[ -d /Library/Audio/Plug-Ins/HAL/MacSound.driver || \
      -d /Library/Audio/Plug-Ins/HAL/MacSoundMic.driver || \
      -d /Library/Audio/Plug-Ins/HAL/MacToolsMic.driver ]]; then
  sudo rm -rf /Library/Audio/Plug-Ins/HAL/MacSound.driver \
    /Library/Audio/Plug-Ins/HAL/MacSoundMic.driver \
    /Library/Audio/Plug-Ins/HAL/MacToolsMic.driver
  sudo killall coreaudiod
fi
rm -f /tmp/io.griplabs.macsound.mic.shared
rm -f "${launch_agent}"
echo "MacTools binaries removed. Settings remain in ~/Library/Application Support/MacTools."
