using System.Diagnostics;
using System.Runtime.InteropServices;

namespace PersonalTools;

internal sealed class ClipboardNative : IDisposable
{
    private readonly nint window;
    private readonly SubclassProc callback;
    private readonly Action changed, hotkey;
    private bool registered;
    public ClipboardNative(nint window, Action changed, Action hotkey)
    {
        this.window = window; this.changed = changed; this.hotkey = hotkey;
        callback = WindowMessage;
        if (!SetWindowSubclass(window, callback, 2, 0)) throw new InvalidOperationException("클립보드 창 연결에 실패했습니다.");
        if (!AddClipboardFormatListener(window))
        { RemoveWindowSubclass(window, callback, 2); throw new InvalidOperationException("클립보드 감지를 시작하지 못했습니다."); }
    }
    public void SetHotkey(uint modifiers, uint key)
    {
        if (registered) UnregisterHotKey(window, 0x5043);
        registered = RegisterHotKey(window, 0x5043, modifiers | 0x4000, key);
        if (!registered) throw new InvalidOperationException("단축키가 다른 앱에서 사용 중입니다. 클립보드 설정에서 변경해 주세요.");
    }
    private nint WindowMessage(nint hwnd, uint message, nuint w, nint l, nuint id, nuint data)
    {
        if (message == 0x31D) changed();
        if (message == 0x312 && w == 0x5043) { hotkey(); return 0; }
        return DefSubclassProc(hwnd, message, w, l);
    }
    public static string SourceApp()
    {
        GetWindowThreadProcessId(GetClipboardOwner(), out uint id);
        try { return id == 0 ? "알 수 없음" : Process.GetProcessById((int)id).ProcessName + ".exe"; }
        catch { return "알 수 없음"; }
    }
    public static string? SourcePath()
    {
        GetWindowThreadProcessId(GetClipboardOwner(), out uint id);
        try { using var process = Process.GetProcessById((int)id); return process.MainModule?.FileName; }
        catch { return null; }
    }
    public static async Task<bool> PasteAsync(nint target)
    {
        if (target == 0 || !IsWindow(target)) return false;
        // Never paste into a different window or while the invocation shortcut is still held.
        if (!SetForegroundWindow(target)) return false;
        for (int i = 0; i < 30; i++)
        {
            if ((GetAsyncKeyState(0x10) & 0x8000) == 0 && (GetAsyncKeyState(0x11) & 0x8000) == 0 && (GetAsyncKeyState(0x12) & 0x8000) == 0) break;
            await Task.Delay(20);
        }
        await Task.Delay(60);
        if (GetForegroundWindow() != target || new[] { 0x10, 0x11, 0x12 }.Any(key => (GetAsyncKeyState(key) & 0x8000) != 0)) return false;
        Input Key(ushort code, bool up) => new() { Type = 1, Keyboard = new() { VirtualKey = code, Flags = up ? 2u : 0 } };
        Input[] inputs = [Key(0x11, false), Key(0x56, false), Key(0x56, true), Key(0x11, true)];
        uint sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>());
        if (sent == inputs.Length) return true;
        // Balance modifiers if Windows accepted only part of the sequence.
        SendInput(2, [Key(0x56, true), Key(0x11, true)], Marshal.SizeOf<Input>());
        return false;
    }
    public void Dispose()
    {
        if (registered) UnregisterHotKey(window, 0x5043);
        RemoveClipboardFormatListener(window);
        RemoveWindowSubclass(window, callback, 2);
    }
    [StructLayout(LayoutKind.Sequential)] private struct KeyboardInput { public ushort VirtualKey, ScanCode; public uint Flags, Time; public nuint ExtraInfo; }
    [StructLayout(LayoutKind.Explicit, Size = 40)] private struct Input { [FieldOffset(0)] public uint Type; [FieldOffset(8)] public KeyboardInput Keyboard; }
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] private delegate nint SubclassProc(nint h, uint m, nuint w, nint l, nuint id, nuint data);
    [DllImport("comctl32.dll")] private static extern bool SetWindowSubclass(nint h, SubclassProc p, nuint id, nuint data);
    [DllImport("comctl32.dll")] private static extern bool RemoveWindowSubclass(nint h, SubclassProc p, nuint id);
    [DllImport("comctl32.dll")] private static extern nint DefSubclassProc(nint h, uint m, nuint w, nint l);
    [DllImport("user32.dll")] private static extern bool AddClipboardFormatListener(nint h);
    [DllImport("user32.dll")] private static extern bool RemoveClipboardFormatListener(nint h);
    [DllImport("user32.dll")] private static extern bool RegisterHotKey(nint h, int id, uint modifiers, uint key);
    [DllImport("user32.dll")] private static extern bool UnregisterHotKey(nint h, int id);
    [DllImport("user32.dll")] private static extern nint GetClipboardOwner();
    [DllImport("user32.dll")] public static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetClipboardSequenceNumber();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint h, out uint id);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint h);
    [DllImport("user32.dll")] private static extern bool IsWindow(nint h);
    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
    [DllImport("user32.dll")] private static extern uint SendInput(uint count, Input[] inputs, int size);
}
