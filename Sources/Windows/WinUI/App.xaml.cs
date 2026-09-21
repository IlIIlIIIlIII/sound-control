using Microsoft.UI.Xaml;
using System.Threading;
using System.Runtime.InteropServices;

namespace SoundControl;
public partial class App : Application
{
    internal AntigravityBridge Bridge { get; } = new(BridgeSettings.LoadOrCreateApiKey());
    private Window? window;
    private Mutex? instance;
    private EventWaitHandle? activate;
    private RegisteredWaitHandle? activationWait;
    public App()
    {
        UnhandledException += (_, e) => LogFailure(e.Exception);
        InitializeComponent();
    }
    private static void LogFailure(Exception error)
    {
        try
        {
            var folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SoundControl");
            Directory.CreateDirectory(folder);
            string detail = "";
            if (GetRestrictedErrorInfo(out var info) == 0 && info is not null)
            {
                info.GetErrorDetails(out var description, out var code, out var restricted, out var capability);
                detail = $"\n{description}\n{restricted}\n{code:X8}";
            }
            File.WriteAllText(Path.Combine(folder, "startup-error.txt"), error + detail);
        }
        catch { /* Never mask the original initialization failure. */ }
    }
    [DllImport("combase.dll")] private static extern int GetRestrictedErrorInfo(out IRestrictedErrorInfo info);
    [ComImport, Guid("82BA7092-4C88-427D-A7BC-16DD93FEB67E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IRestrictedErrorInfo
    {
        void GetErrorDetails([MarshalAs(UnmanagedType.BStr)] out string description, out int error,
            [MarshalAs(UnmanagedType.BStr)] out string restricted, [MarshalAs(UnmanagedType.BStr)] out string capability);
        void GetReference([MarshalAs(UnmanagedType.BStr)] out string reference);
    }
    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        bool inTray = Environment.GetCommandLineArgs().Contains("--startup", StringComparer.OrdinalIgnoreCase);
        instance = new Mutex(true, @"Local\SoundControlWinUI", out bool first);
        activate = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\SoundControlWinUIActivate");
        if (!first) { if (!inTray) activate.Set(); Exit(); return; }
        MainWindow main;
        try { main = new MainWindow(); }
        catch (Exception error) { LogFailure(error); throw; }
        window = main;
        activationWait = ThreadPool.RegisterWaitForSingleObject(activate,
            (_, _) => main.DispatcherQueue.TryEnqueue(main.Show), null, Timeout.Infinite, false);
        main.Closed += (_, _) =>
        {
            activationWait?.Unregister(null);
            activate?.Dispose();
            instance?.ReleaseMutex();
            instance?.Dispose();
        };
        main.Start(inTray);
    }
}
