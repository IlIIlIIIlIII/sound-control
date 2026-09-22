#!/bin/zsh
set -euo pipefail

if [[ "${1:-}" != "--confirm-system-change" ]]; then
  echo "Usage: $0 --confirm-system-change"
  echo "Installs PersonalTools.app and its one-channel virtual microphone, then starts the user engine."
  exit 2
fi

repo_dir="${0:A:h:h}"
app_source="${repo_dir}/build/PersonalTools.app"
engine_source="${app_source}/Contents/Helpers/PersonalToolsEngine.app"
mic_driver_source="${app_source}/Contents/Resources/PersonalToolsMic.driver"
mic_driver_target="/Library/Audio/Plug-Ins/HAL/PersonalToolsMic.driver"
launch_agent_source="${repo_dir}/Resources/io.griplabs.personaltools.engine.plist"
launch_agent_target="/Users/sunggu/Library/LaunchAgents/io.griplabs.personaltools.engine.plist"
app_target="/Users/sunggu/Applications/PersonalTools.app"
settings_target="/Users/sunggu/Library/Application Support/PersonalTools"
engine_target="${settings_target}/PersonalToolsEngine.app"

if [[ ! -d "${app_source}" || ! -d "${mic_driver_source}" ]]; then
  echo "Release app not found. Run: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build"
  exit 1
fi
/usr/bin/codesign --verify --deep --strict "${engine_source}"
if ! /usr/bin/codesign -d -r- "${engine_source}" 2>&1 | \
    /usr/bin/grep -q 'identifier "io.griplabs.personaltools.engine"'; then
  echo "Signed PersonalToolsEngine helper not found. Rebuild PersonalToolsPackage before installing."
  exit 1
fi

mkdir -p /Users/sunggu/Applications
launchctl bootout gui/"$(id -u)" "${launch_agent_target}" 2>/dev/null || true
for _attempt in {1..30}; do
  if ! pgrep -x PersonalToolsEngine >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
pkill -9 -x PersonalToolsEngine 2>/dev/null || true
pkill -x PersonalToolsEngine 2>/dev/null || true
pkill -x PersonalTools 2>/dev/null || true
rm -rf "${app_target}"
ditto "${app_source}" "${app_target}"
rm -rf "${engine_target}"
ditto "${engine_source}" "${engine_target}"
mkdir -p /Users/sunggu/Library/LaunchAgents
cp "${launch_agent_source}" "${launch_agent_target}"
sudo rm -rf "${mic_driver_target}"
sudo ditto "${mic_driver_source}" "${mic_driver_target}"
sudo chown -R root:wheel "${mic_driver_target}"
sudo chmod -R go-w "${mic_driver_target}"
sudo killall -9 coreaudiod audiomxd audioaccessoryd 2>/dev/null || true
sleep 2
launchctl enable gui/"$(id -u)"/io.griplabs.personaltools.engine
launchctl bootstrap gui/"$(id -u)" "${launch_agent_target}"
/usr/bin/killall ControlCenter 2>/dev/null || true
echo "Installed. PersonalTools keeps the selected direct output, exposes M2 input 1, enables speaker echo cancellation, and keeps display shortcuts active."
