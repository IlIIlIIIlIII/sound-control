# Clipboard bottom panel

The default hotkey is **Ctrl+Shift+V**. The panel opens at the bottom of the
monitor under the pointer, using its work area and DPI (340 DIP high, clamped
to the available height). The paste target remains the foreground window from
before invocation. Escape, the hotkey again, an outside click or activation of
another app hides the panel. Outside clicks are observed, never consumed.

## Using the panel

- Switch between local search and AI search next to the search field. Local
  search waits 200ms after typing; AI search requires Enter or the search button.
- Local filters combine copied process, local calendar date and content format.
  Multiple processes/formats are ORed within their group; groups are ANDed.
  No checked processes/formats means all. Date endpoints are inclusive.
- A complete HTTP(S) URL is shown as URL; text containing a link remains text.
  Original text payloads are retained for copying and pasting.
- Use **+** to create a collection. Right-click its tab to rename or remove it.
  The card menu assigns a record to multiple collections. Removing a collection
  leaves the records intact.
- Double-click a card or press Enter on a selected card to paste. Its menu also
  offers copy, preview, pin/unpin and delete. Copy keeps the panel open.
- The same cards and filters are available inside the main app; only the
  standalone panel dismisses on outside clicks.

## Persistence and search lifecycle

`categories.json` in the archive stores category IDs, names and record membership
in one atomic replacement. Archive relocation copies and verifies this file.
Old archives load without categories; old text entries are classified lazily
from their full payload and cached for the session. New entries include optional
`ContentFormat` metadata without changing their existing `Kind` or payload.

AI search receives the selected collection's allowed IDs/directories and checks
returned IDs against the current archive. This is search scoping, not a separate
filesystem sandbox per collection. Invalid scoped results suppress the generated
summary as well as dropping invalid cards. Changing query/mode/collection, closing
the panel or changing scoped membership cancels the request. Revision checks
prevent older requests from repainting the current UI.

## Windows personalization and materials

The popup uses the built-in `DesktopAcrylicBackdrop`, with a transparent XAML
root so the actual desktop blur is visible. Its tint/fallback colors are not
overridden: WinUI manages live light/dark changes and the solid-color fallbacks
for transparency disabled, high contrast, battery saver and unsupported rendering.
The embedded main-app page uses the system theme background instead.

All card, text, stroke and accent colors use `ThemeResource` brushes. No app-wide
`RequestedTheme` or `SystemAccentColor` override is installed. Windows accent
selection therefore controls the toggle, selected states, icons and accent button.
The native popup border follows `ActualThemeChanged`; DWM owns window rounding.
Cards (18 DIP), search (16 DIP) and buttons (12 DIP) have a softer rounded shape
while preserving native WinUI focus, hover and accessibility behavior.

Microsoft guidance consulted:

- [Theming in Windows apps](https://learn.microsoft.com/en-us/windows/apps/develop/ui/theming)
- [Materials in Windows apps](https://learn.microsoft.com/en-us/windows/apps/develop/ui/materials)
- [System backdrops](https://learn.microsoft.com/en-us/windows/apps/develop/ui/system-backdrops)
- [Rounded desktop window corners](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-rounded-corners)
- [Contrast themes](https://learn.microsoft.com/en-us/windows/apps/design/accessibility/high-contrast-themes)

## Validation

Run `dotnet run --project Tests/Clipboard/ClipboardTests.csproj -c Release`.
The fixture uses temporary archives and a local child process, without a live AI
service. Coverage includes legacy payloads, full-text boundaries, URL detection,
filter combinations/local date boundaries, category persistence/relocation,
scoped AI result validation, cancellation, and panel placement at different DPI
and negative monitor origins. These tests also run in Windows CI.

Build with `dotnet build Sources/Windows/WinUI/PersonalToolsWindows.csproj -c Release -p:Platform=x64`.

Manual regression scenarios:

1. Invoke from two monitors with different scaling and taskbar locations. Move
   the pointer to the other monitor while keeping an editor active: the panel
   follows the pointer, and paste returns to the editor.
2. Test outside clicks (including the desktop/taskbar), Escape, Alt+Tab and rapid
   hotkey toggling. The outside target receives its original click. Flyouts,
   calendar controls, dialogs and the owned folder picker remain usable.
3. Exercise local filters, an empty result, AI failure/cancel, and switch modes or
   close/reopen before an AI request completes. Old results must never reappear.
4. Open previews, assign several categories, rename/delete a category, and restart.
   Verify the main app and popup agree on updated membership and pin state.
5. Scroll many image cards; recycled cards must not retain another image. Check
   keyboard navigation, light/dark/high-contrast themes, a narrow main window and
   Windows animations disabled. Test target-app paste failure and manual Ctrl+V.

Validated locally: 64 automated checks; WinUI x64 build/publish; actual dark-theme
desktop Acrylic rendering with visible background blur, rounded search/cards,
thumbnails, normal/AI switching, internal filter clicks
and filtering 18 records down to 3 image records. Physical mixed-DPI monitors,
other themes, owned folder picker and paste into an external editor still require
the manual scenarios above.

## Search shortcuts and file retrieval (2026-09-22)
- In the search box, Enter always selects normal search; Shift+Enter selects and
  runs Antigravity AI search. Card-focused Enter still pastes the selected item.
- Horizontal wheel/trackpad direction is reversed. Scrollbar dragging is unchanged.
- AI receives an ephemeral scoped manifest with original filenames, stored paths,
  timestamps and source apps. It must read the actual attachments for content
  questions, including dot files and text beyond the preview. It must distinguish
  filename matches from verified content and report unsupported formats.
- Absolute archive/manifest paths prevent CLI tools from searching their own
  default directory. Terminal commands and external folders are excluded by the
  retrieval prompt; the CLI sandbox and permission checks remain enabled.
- Structured CLI results are read from structured_output when present. Empty
  permission-denied responses produce an actionable message. Temporary manifests
  are removed on completion, error, cancellation and timeout.
- Live verification uses synthetic files in a separate temporary archive, never
  the user's real clipboard history. Run the clipboard tests with -- --live.

Latest validation:
- 69 deterministic clipboard checks and 32 bridge checks pass; WinUI x64 publish passes.
- Real Antigravity read the synthetic clipboard text and returned CORAL-729 with
  the correct ID. It also distinguished two copied notes.txt files and found
  AURORA-582 and 김서윤 after 9,000 characters, returning only the matching record.
- The subsequent .env.example live search was denied by Antigravity's headless
  tool-permission policy. No permissions were weakened. This live case remains
  unverified, and the later live negative-scope case did not run. Deterministic
  scope exclusion/cancellation checks pass. PDF/Office/OCR are not validated.
- Ctrl+F in the clipboard page/panel focuses the search box and selects its existing text. Modal dialogs and the owned folder picker keep their own keyboard handling.
