using System.Runtime.InteropServices;

namespace SoundControl;
// Only shell integration uses HWND APIs. All visible settings and scrolling are WinUI.
internal sealed class TrayIcon : IDisposable
{
    private const uint CallbackMessage = 0x8001;
    private readonly nint window;
    private readonly Action show, quit;
    private readonly SubclassProc callback;
    private readonly uint taskbarCreated;
    private NotifyIconData icon;
    private readonly nint customIcon;
    public bool Available { get; private set; }
    public TrayIcon(nint window, Action show, Action quit)
    {
        this.window = window; this.show = show; this.quit = quit;
        callback = WindowMessage;
        taskbarCreated = RegisterWindowMessage("TaskbarCreated");
        customIcon = LoadImage(0, Path.Combine(AppContext.BaseDirectory, "Assets", "SoundControl.ico"), 1, 32, 32, 0x10);
        icon = new NotifyIconData { Size = (uint)Marshal.SizeOf<NotifyIconData>(), Window = window, Id = 1,
            Flags = 7, CallbackMessage = CallbackMessage, Icon = customIcon != 0 ? customIcon : LoadIcon(0, (nint)32512), Tip = "Personal Tools",
            Info = "", InfoTitle = "" };
        if (!SetWindowSubclass(window, callback, 1, 0)) return;
        Available = Shell_NotifyIcon(0, ref icon);
    }
    private nint WindowMessage(nint hwnd, uint message, nuint wParam, nint lParam, nuint id, nuint data)
    {
        if (message == taskbarCreated) { Available = Shell_NotifyIcon(0, ref icon); return 0; }
        if (message == CallbackMessage)
        {
            uint input = (uint)lParam;
            if (input == 0x203) show();
            if (input == 0x205)
            {
                nint menu = CreatePopupMenu();
                try
                {
                    AppendMenu(menu, 0, 1, "Personal Tools 열기");
                    AppendMenu(menu, 0, 2, "앱 종료 (실행 중인 도구 중지)");
                    GetCursorPos(out Point point); SetForegroundWindow(hwnd);
                    int chosen = TrackPopupMenu(menu, 0x102, point.X, point.Y, 0, hwnd, 0);
                    if (chosen == 1) show();
                    if (chosen == 2) quit();
                    PostMessage(hwnd, 0, 0, 0);
                }
                finally { DestroyMenu(menu); }
            }
            return 0;
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }
    public void Dispose()
    {
        Shell_NotifyIcon(2, ref icon);
        RemoveWindowSubclass(window, callback, 1);
        if (customIcon != 0) DestroyIcon(customIcon);
        Available = false;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NotifyIconData
    {
        public uint Size; public nint Window; public uint Id, Flags, CallbackMessage; public nint Icon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Tip;
        public uint State, StateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Info;
        public uint Version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string InfoTitle;
        public uint InfoFlags; public Guid Guid; public nint BalloonIcon;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X, Y; }
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate nint SubclassProc(nint h, uint m, nuint w, nint l, nuint id, nuint data);
    [DllImport("comctl32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool SetWindowSubclass(nint h, SubclassProc p, nuint id, nuint data);
    [DllImport("comctl32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool RemoveWindowSubclass(nint h, SubclassProc p, nuint id);
    [DllImport("comctl32.dll")] private static extern nint DefSubclassProc(nint h, uint m, nuint w, nint l);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool Shell_NotifyIcon(uint m, ref NotifyIconData data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern nint LoadIcon(nint instance, nint name);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern nint LoadImage(nint instance, string name, uint type, int width, int height, uint flags);
    [DllImport("user32.dll")] private static extern bool DestroyIcon(nint icon);
    [DllImport("user32.dll")] private static extern nint CreatePopupMenu();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool AppendMenu(nint menu, uint flags, nuint id, string label);
    [DllImport("user32.dll")] private static extern bool DestroyMenu(nint menu);
    [DllImport("user32.dll")] private static extern int TrackPopupMenu(nint menu, uint flags, int x, int y, int reserved, nint window, nint rect);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint window);
    [DllImport("user32.dll")] private static extern bool PostMessage(nint window, uint message, nuint w, nint l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern uint RegisterWindowMessage(string name);
}
