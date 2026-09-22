using System.Runtime.InteropServices;
using System.Text;

namespace SoundControl;

internal readonly record struct ClipboardPanelBounds(int X, int Y, int Width, int Height)
{
    internal static ClipboardPanelBounds AtBottom(int x, int y, int width, int height, uint dpi, bool floating = false)
    {
        if (floating)
        {
            int gap = Math.Min(Math.Min(width / 4, height / 4), (int)Math.Round(12 * (dpi == 0 ? 96 : dpi) / 96d));
            return AtBottom(x + gap, y + gap, width - gap * 2, height - gap * 2, dpi);
        }
        int panelHeight = Math.Min(height, Math.Max(1, (int)Math.Round(340 * (dpi == 0 ? 96 : dpi) / 96d)));
        return new(x, y + height - panelHeight, width, panelHeight);
    }
}

// Scoped to the visible panel. The hook observes clicks and always forwards them unchanged.
internal sealed class ClipboardPanelNative : IDisposable
{
    private readonly nint window;
    private readonly Action outsideClick;
    private readonly HookProc callback;
    private nint hook;
    internal ClipboardPanelNative(nint window, Action outsideClick)
    { this.window = window; this.outsideClick = outsideClick; callback = MouseHook; }
    internal static ClipboardPanelBounds CursorBounds()
    {
        GetCursorPos(out var point);
        var monitor = MonitorFromPoint(point, 2);
        var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
        if (!GetMonitorInfo(monitor, ref info)) throw new InvalidOperationException("모니터 작업 영역을 확인하지 못했습니다.");
        uint dpi = 96;
        if (GetDpiForMonitor(monitor, 0, out uint x, out _) == 0) dpi = x;
        return ClipboardPanelBounds.AtBottom(info.Work.Left, info.Work.Top, info.Work.Right - info.Work.Left, info.Work.Bottom - info.Work.Top, dpi, floating: true);
    }
    internal bool OwnsWindow(nint candidate)
    {
        if (candidate == 0) return false;
        var root = GetAncestor(candidate, 2);
        for (nint cursor = root; cursor != 0; cursor = GetWindow(cursor, 4))
            if (cursor == window) return true;
        // XAML flyouts can be hosted by an unowned popup HWND on the panel's UI thread.
        if (GetWindowThreadProcessId(root, out _) == GetWindowThreadProcessId(window, out _) && root != 0)
        {
            var name = new StringBuilder(256); GetClassName(root, name, name.Capacity);
            return name.ToString().Contains("Popup", StringComparison.OrdinalIgnoreCase);
        }
        return false;
    }
    internal void Start()
    {
        if (hook != 0) return;
        hook = SetWindowsHookEx(14, callback, GetModuleHandle(null), 0);
        if (hook == 0) throw new InvalidOperationException("패널 외부 클릭 감지를 시작하지 못했습니다.");
    }
    internal void Stop() { if (hook != 0) { UnhookWindowsHookEx(hook); hook = 0; } }
    private nint MouseHook(int code, nuint message, nint data)
    {
        if (code >= 0 && message is 0x201 or 0x204 or 0x207 or 0x20B)
        {
            var click = Marshal.PtrToStructure<MouseData>(data);
            if (!OwnsWindow(WindowFromPoint(click.Point))) outsideClick();
        }
        return CallNextHookEx(hook, code, message, data);
    }
    internal void ClipReveal(int width, int height)
    {
        var region = CreateRectRgn(0, 0, width, Math.Max(1, height));
        if (SetWindowRgn(window, region, true) == 0) DeleteObject(region);
    }
    internal void ClearClip() => SetWindowRgn(window, 0, true);
    internal void ApplySystemAppearance(bool dark)
    {
        // Use DWM's rounded window and themed border instead of an alpha-layered
        // window or a persistent custom region, which disable system rounding.
        int corners = 2, darkMode = dark ? 1 : 0;
        DwmSetWindowAttribute(window, 33, ref corners, sizeof(int));
        DwmSetWindowAttribute(window, 20, ref darkMode, sizeof(int));
    }
    public void Dispose() => Stop();
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct MonitorInfo { public int Size; public Rect Monitor, Work; public uint Flags; }
    [StructLayout(LayoutKind.Sequential)] private struct MouseData { public Point Point; public uint Mouse, Flags, Time; public nuint Extra; }
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate nint HookProc(int code, nuint message, nint data);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] private static extern nint MonitorFromPoint(Point point, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool GetMonitorInfo(nint monitor, ref MonitorInfo info);
    [DllImport("shcore.dll")] private static extern int GetDpiForMonitor(nint monitor, int type, out uint x, out uint y);
    [DllImport("user32.dll")] private static extern nint WindowFromPoint(Point point);
    [DllImport("user32.dll")] private static extern nint GetAncestor(nint window, uint flags);
    [DllImport("user32.dll")] private static extern nint GetWindow(nint window, uint command);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint window, out uint process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(nint window, StringBuilder name, int size);
    [DllImport("user32.dll")] private static extern nint SetWindowsHookEx(int id, HookProc proc, nint module, uint thread);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern nint GetModuleHandle(string? name);
    [DllImport("user32.dll")] private static extern bool UnhookWindowsHookEx(nint hook);
    [DllImport("user32.dll")] private static extern nint CallNextHookEx(nint hook, int code, nuint message, nint data);
    [DllImport("gdi32.dll")] private static extern nint CreateRectRgn(int left, int top, int right, int bottom);
    [DllImport("gdi32.dll")] private static extern bool DeleteObject(nint obj);
    [DllImport("user32.dll")] private static extern int SetWindowRgn(nint window, nint region, bool redraw);
    [DllImport("dwmapi.dll")] private static extern int DwmSetWindowAttribute(nint window, uint attribute, ref int value, int size);
}
