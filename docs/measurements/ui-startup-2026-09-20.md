# WinUI branding and sign-in verification

- Published the Release win-x64 self-contained UI successfully.
- Moved branding out of the NavigationView hamburger row into PaneCustomContent. Expanded navigation hides the toggle; compact navigation retains it.
- Generated PNG with built-in imagegen and encoded seven ICO sizes. Executable, AppWindow, tray and sidebar use the new artwork.
- Computer Use confirmed the installed Sound and Processing Status screens with aligned branding and the titlebar icon.
- Installed UI DLL SHA256: `B4D9CB178E7A1C1B82ADA4A44E6698678BB195D5CD8F09862B875901D9CC7763`.
- HKCU Run/SoundControl: `"C:\Program Files\SoundControl\ui\SoundControlWindows.exe" --startup`.
- Installed `--startup` launched PID 43752 with MainWindowHandle 0. Repeating `--startup` retained the same single hidden process. Normal launch displayed that same process's window.
- No Windows logout/reboot was performed. Native audio bridge hash stayed unchanged.
