#!/bin/zsh
set -euo pipefail

if [[ "${1:-}" != "--confirm-system-change" ]]; then
  echo "Usage: $0 --confirm-system-change"
  echo "Stops PersonalTools and removes its app, microphone driver, old driver, and LaunchAgent. Settings are preserved."
  exit 2
fi

launch_agent="/Users/sunggu/Library/LaunchAgents/io.griplabs.personaltools.engine.plist"
launchctl bootout gui/"$(id -u)" "${launch_agent}" 2>/dev/null || true
rm -rf /Users/sunggu/Applications/PersonalTools.app
if [[ -d /Library/Audio/Plug-Ins/HAL/PersonalTools.driver || \
      -d /Library/Audio/Plug-Ins/HAL/PersonalToolsMic.driver ]]; then
  sudo rm -rf /Library/Audio/Plug-Ins/HAL/PersonalTools.driver \
    /Library/Audio/Plug-Ins/HAL/PersonalToolsMic.driver
  sudo killall coreaudiod
fi
rm -f /tmp/io.griplabs.personaltools.mic.shared
rm -f "${launch_agent}"
echo "PersonalTools binaries removed. Settings remain in ~/Library/Application Support/PersonalTools."
