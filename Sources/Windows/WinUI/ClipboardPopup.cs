using System.Diagnostics;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;
using Windows.Graphics;

namespace PersonalTools;

internal sealed class ClipboardPopup : Window
{
    private readonly ClipboardController controller;
    private readonly ClipboardPage page;
    private readonly ClipboardPanelNative native;
    private readonly DispatcherQueueTimer animation;
    private readonly Stopwatch animationClock = new();
    private ClipboardPanelBounds bounds;
    private nint target;
    private bool visible, exiting;
    private int presentation;
    public ClipboardPopup(ClipboardController controller)
    {
        this.controller = controller;
        Title = "클립보드 · Personal Tools";
        SystemBackdrop = new ClipboardAcrylicBackdrop();
        page = new ClipboardPage(); Content = page;
        page.UseDesktopAcrylic();
        page.Configure(controller, this); page.PasteRequested = PasteAsync; page.Dismiss = Hide;
        page.Suspend();
        native = new(WindowNative.GetWindowHandle(this), QueueDismiss);
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.SetBorderAndTitleBar(false, false);
            presenter.IsAlwaysOnTop = true; presenter.IsMinimizable = false;
            presenter.IsMaximizable = false; presenter.IsResizable = false;
        }
        AppWindow.IsShownInSwitchers = false;
        page.ActualThemeChanged += (_, _) => native.ApplySystemAppearance(page.ActualTheme == ElementTheme.Dark);
        native.ApplySystemAppearance(page.ActualTheme == ElementTheme.Dark);
        animation = DispatcherQueue.CreateTimer(); animation.Interval = TimeSpan.FromMilliseconds(16);
        animation.Tick += (_, _) => Animate();
        Activated += (_, args) =>
        {
            if (args.WindowActivationState == WindowActivationState.Deactivated)
                DispatcherQueue.TryEnqueue(() =>
                {
                    if (visible && !page.OwnedPickerOpen && !native.OwnsWindow(ClipboardNative.GetForegroundWindow())) Hide();
                });
        };
        AppWindow.Closing += (_, e) => { if (!exiting) { e.Cancel = true; Hide(); } };
    }
    private void QueueDismiss()
    {
        int current = presentation;
        if (page.OwnedPickerOpen) return;
        DispatcherQueue.TryEnqueue(() => { if (visible && current == presentation && !page.OwnedPickerOpen) Hide(); });
    }
    public void Toggle()
    {
        if (visible) { Hide(); return; }
        target = ClipboardNative.GetForegroundWindow();
        bounds = ClipboardPanelNative.CursorBounds();
        Show(true);
    }
    private void Show(bool animate)
    {
        ++presentation;
        animation.Stop(); native.ClearClip(); native.ApplySystemAppearance(page.ActualTheme == ElementTheme.Dark);
        AppWindow.MoveAndResize(new RectInt32(bounds.X, bounds.Y, bounds.Width, bounds.Height));
        bool motion = animate && new Windows.UI.ViewManagement.UISettings().AnimationsEnabled;
        if (motion)
        {
            native.ClipReveal(bounds.Width, 1);
            AppWindow.Move(new PointInt32(bounds.X, bounds.Y + bounds.Height - 1));
        }
        visible = true; AppWindow.Show(); Activate(); page.FocusSearch();
        try { native.Start(); }
        catch { Hide(); throw; }
        if (motion) { animationClock.Restart(); animation.Start(); }
    }
    private void Animate()
    {
        double t = Math.Clamp(animationClock.Elapsed.TotalMilliseconds / 180, 0, 1);
        int reveal = Math.Max(1, (int)Math.Round(bounds.Height * (1 - Math.Pow(1 - t, 3))));
        native.ClipReveal(bounds.Width, reveal);
        AppWindow.Move(new PointInt32(bounds.X, bounds.Y + bounds.Height - reveal));
        if (t >= 1)
        {
            animation.Stop(); native.ClearClip();
            native.ApplySystemAppearance(page.ActualTheme == ElementTheme.Dark);
        }
    }
    private void Hide()
    {
        ++presentation; visible = false; animation.Stop(); native.Stop();
        page.Suspend(); AppWindow.Hide();
    }
    private async Task PasteAsync(ClipEntry entry)
    {
        int current = presentation;
        await controller.CopyAsync(entry);
        // An outside click during an asynchronous copy must never steal the new focus.
        if (!visible || current != presentation) return;
        Hide(); current = presentation;
        if (!await ClipboardNative.PasteAsync(target) && current == presentation)
        {
            Show(false);
            throw new InvalidOperationException("자동 붙여넣기를 완료하지 못했습니다. 대상 앱에서 Ctrl+V를 눌러 주세요.");
        }
    }
    public async Task ShutdownAsync()
    {
        exiting = true; Hide(); native.Dispose(); await page.ShutdownAsync(); Close();
    }
}
