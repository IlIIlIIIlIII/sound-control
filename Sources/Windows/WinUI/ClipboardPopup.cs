using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace SoundControl;

internal sealed class ClipboardPopup : Window
{
    private readonly ClipboardController controller;
    private readonly ClipboardPage page;
    private nint target;
    private bool visible, exiting;
    public ClipboardPopup(ClipboardController controller)
    {
        this.controller = controller;
        Title = "클립보드 · Personal Tools";
        SystemBackdrop = new Microsoft.UI.Xaml.Media.MicaBackdrop();
        page = new ClipboardPage(); Content = page;
        page.Configure(controller, this); page.PasteRequested = PasteAsync; page.Dismiss = Hide;
        if (AppWindow.Presenter is OverlappedPresenter presenter) { presenter.IsAlwaysOnTop = true; presenter.IsMinimizable = false; }
        AppWindow.Closing += (_, e) => { if (!exiting) { e.Cancel = true; Hide(); } };
    }
    public void Toggle()
    {
        if (visible) { Hide(); return; }
        target = ClipboardNative.GetForegroundWindow();
        var id = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(target);
        var area = DisplayArea.GetFromWindowId(id, DisplayAreaFallback.Primary).WorkArea;
        int width = Math.Min(820, area.Width), height = Math.Min(680, area.Height);
        AppWindow.MoveAndResize(new Windows.Graphics.RectInt32(area.X + (area.Width - width) / 2, area.Y + (area.Height - height) / 2, width, height));
        visible = true; AppWindow.Show(); Activate(); page.FocusSearch();
    }
    private void Hide() { visible = false; AppWindow.Hide(); }
    private async Task PasteAsync(ClipEntry entry)
    {
        await controller.CopyAsync(entry); Hide();
        if (!await ClipboardNative.PasteAsync(target))
        {
            AppWindow.Show(); Activate(); visible = true;
            // Copy already succeeded; leave the payload available for manual Ctrl+V.
            throw new InvalidOperationException("자동 붙여넣기를 완료하지 못했습니다. 대상 앱에서 Ctrl+V를 눌러 주세요.");
        }
    }
    public async Task ShutdownAsync() { exiting = true; await page.ShutdownAsync(); Close(); }
}
