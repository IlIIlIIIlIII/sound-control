#!/bin/zsh
set -euo pipefail

if [[ "${1:-}" != "--confirm-system-change" ]]; then
  echo "Usage: $0 --confirm-system-change"
  echo "Installs MacTools.app and its one-channel virtual microphone, then starts the user engine."
  exit 2
fi

repo_dir="${0:A:h:h}"
app_source="${repo_dir}/build/MacTools.app"
engine_source="${app_source}/Contents/Helpers/MacToolsEngine.app"
mic_driver_source="${app_source}/Contents/Resources/MacToolsMic.driver"
mic_driver_target="/Library/Audio/Plug-Ins/HAL/MacToolsMic.driver"
launch_agent_source="${repo_dir}/Resources/io.griplabs.macsound.engine.plist"
launch_agent_target="/Users/sunggu/Library/LaunchAgents/io.griplabs.macsound.engine.plist"
app_target="/Users/sunggu/Applications/MacTools.app"
legacy_app_target="/Users/sunggu/Applications/MacSound.app"
settings_target="/Users/sunggu/Library/Application Support/MacTools"
engine_target="${settings_target}/MacToolsEngine.app"
legacy_settings="/Users/sunggu/Library/Application Support/MacSound"

if [[ ! -d "${app_source}" || ! -d "${mic_driver_source}" ]]; then
  echo "Release app not found. Run: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build"
  exit 1
fi
/usr/bin/codesign --verify --deep --strict "${engine_source}"
if ! /usr/bin/codesign -d -r- "${engine_source}" 2>&1 | \
    /usr/bin/grep -q 'identifier "io.griplabs.macsound.engine"'; then
  echo "Signed MacToolsEngine helper not found. Rebuild MacToolsPackage before installing."
  exit 1
fi

mkdir -p /Users/sunggu/Applications
launchctl bootout gui/"$(id -u)" "${launch_agent_target}" 2>/dev/null || true
for _attempt in {1..30}; do
  if ! pgrep -x MacToolsEngine >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
pkill -9 -x MacToolsEngine 2>/dev/null || true
pkill -x MacSoundEngine 2>/dev/null || true
pkill -x MacTools 2>/dev/null || true
pkill -x MacSound 2>/dev/null || true
if [[ ! -d "${settings_target}" && -d "${legacy_settings}" ]]; then
  ditto "${legacy_settings}" "${settings_target}"
  rm -f "${settings_target}/status.json" \
    "${settings_target}/mic-status.json" \
    "${settings_target}/display-status.json" \
    "${settings_target}/display-recovery.json"
fi
rm -rf "${app_target}" "${legacy_app_target}"
ditto "${app_source}" "${app_target}"
rm -rf "${engine_target}"
ditto "${engine_source}" "${engine_target}"
mkdir -p /Users/sunggu/Library/LaunchAgents
cp "${launch_agent_source}" "${launch_agent_target}"
sudo rm -rf /Library/Audio/Plug-Ins/HAL/MacSound.driver \
  /Library/Audio/Plug-Ins/HAL/MacSoundMic.driver "${mic_driver_target}"
sudo ditto "${mic_driver_source}" "${mic_driver_target}"
sudo chown -R root:wheel "${mic_driver_target}"
sudo chmod -R go-w "${mic_driver_target}"
sudo killall -9 coreaudiod audiomxd audioaccessoryd 2>/dev/null || true
sleep 2
launchctl enable gui/"$(id -u)"/io.griplabs.macsound.engine
launchctl bootstrap gui/"$(id -u)" "${launch_agent_target}"
/usr/bin/killall ControlCenter 2>/dev/null || true
echo "Installed. MacTools keeps the selected direct output, exposes M2 input 1, enables speaker echo cancellation, and keeps display shortcuts active."
