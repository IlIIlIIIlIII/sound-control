using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Windowing;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace SoundControl;
public sealed partial class MainWindow : Window
{
    private ClipboardController? clipboard;
    private ClipboardPopup? clipboardPopup;
    private Task? clipboardStartup;
    private AudioService? audio;
    private AudioSnapshot? current;
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(1) };
    private TrayIcon? tray;
    private bool updating, refreshing, busy, quitting, closed;
    private bool bridgeStartupRequested;
    private static string PackageDirectory => Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, ".."));
    private static string DataDirectory => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "SoundControl");
    private static string BackupPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "SoundControl", "installation-backup.clixml");

    public MainWindow()
    {
        InitializeComponent();
        Title = "Personal Tools";
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "SoundControl.ico"));
        UpdateTitleTheme();
        Root.ActualThemeChanged += (_, _) => UpdateTitleTheme();
        var dpi = GetDpiForWindow(WindowNative.GetWindowHandle(this)) / 96.0;
        var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        AppWindow.Resize(new Windows.Graphics.SizeInt32(
            Math.Min((int)(1120 * dpi), area.Width - 40), Math.Min((int)(850 * dpi), area.Height - 40)));
        Navigation.SelectedItem = SoundNav;
        clipboard = new ClipboardController(((App)Application.Current).Bridge, DispatcherQueue);
        ClipsPage.Configure(clipboard, this);
        clipboard.TogglePopup += () => { clipboardPopup ??= new ClipboardPopup(clipboard); clipboardPopup.Toggle(); };
        ClipsPage.PasteRequested = async item => { await clipboard.CopyAsync(item); ShowMessage("복사했습니다", "원래 앱에 바로 붙여넣으려면 전역 단축키로 클립보드를 여세요.", InfoBarSeverity.Informational); };
        try
        {
            tray = new TrayIcon(WindowNative.GetWindowHandle(this),
                () => DispatcherQueue.TryEnqueue(Show), () => DispatcherQueue.TryEnqueue(Quit));
            audio = new AudioService();
        }
        catch (Exception e) { ShowMessage("시작하지 못했습니다", e.Message, InfoBarSeverity.Error); }
        AppWindow.Closing += (_, e) =>
        {
            if (!quitting && tray?.Available == true) { e.Cancel = true; AppWindow.Hide(); }
            else if (!quitting) { e.Cancel = true; Quit(); }
        };
        Closed += (_, _) => { closed = true; timer.Stop(); tray?.Dispose(); audio?.Dispose(); };
        timer.Tick += async (_, _) => await RefreshAsync();
        Root.Loaded += async (_, _) => { await RefreshAsync(); timer.Start(); };
    }
    public void Show() { AppWindow.Show(); Activate(); }
    public void Start(bool inTray)
    {
        clipboardStartup ??= clipboard!.StartAsync(WindowNative.GetWindowHandle(this));
        if (!inTray || tray?.Available != true) Show();
        else timer.Start();
        // Start even when the window stays hidden in the tray; tab loading is not required.
        if (!bridgeStartupRequested)
        {
            bridgeStartupRequested = true;
            _ = BridgePage.StartBridgeAsync();
        }
    }
    private void NavigationDisplayModeChanged(NavigationView sender, NavigationViewDisplayModeChangedEventArgs args)
    {
        bool expanded = args.DisplayMode == NavigationViewDisplayMode.Expanded;
        sender.IsPaneToggleButtonVisible = !expanded;
        if (BrandHeader is not null) BrandHeader.Visibility = expanded ? Visibility.Visible : Visibility.Collapsed;
    }
    private void UpdateTitleTheme()
    {
        int dark = Root.ActualTheme == ElementTheme.Dark ? 1 : 0;
        DwmSetWindowAttribute(WindowNative.GetWindowHandle(this), 20, ref dark, sizeof(int));
    }
    private async void Quit()
    {
        if (quitting) return;
        quitting = true;
        try
        {
            if (clipboardStartup is not null) await clipboardStartup;
            await ClipsPage.ShutdownAsync();
            if (clipboardPopup is not null) await clipboardPopup.ShutdownAsync();
            if (clipboard is not null) await clipboard.DisposeAsync();
            await BridgePage.ShutdownAsync();
        }
        finally { Close(); }
    }
    private void Navigate(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (SoundScroll is null || StatusScroll is null || BridgePage is null || ClipsPage is null) return;
        var tag = (args.SelectedItem as NavigationViewItem)?.Tag as string;
        if (tag == "exit") { Quit(); return; }
        SoundScroll.Visibility = tag == "sound" ? Visibility.Visible : Visibility.Collapsed;
        StatusScroll.Visibility = tag == "status" ? Visibility.Visible : Visibility.Collapsed;
        BridgePage.Visibility = tag == "antigravity" ? Visibility.Visible : Visibility.Collapsed;
        ClipsPage.Visibility = tag == "clipboard" ? Visibility.Visible : Visibility.Collapsed;
        AudioFooter.Visibility = tag is "antigravity" or "clipboard" ? Visibility.Collapsed : Visibility.Visible;
    }
    private void ProfileSizeChanged(object sender, SizeChangedEventArgs e)
    {
        bool narrow = e.NewSize.Width < 600;
        Grid.SetColumn(RightProfile, narrow ? 0 : 1);
        Grid.SetRow(RightProfile, narrow ? 1 : 0);
        RightColumn.Width = narrow ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
    }
    private async Task RefreshAsync()
    {
        if (refreshing || closed || audio is null) return;
        refreshing = true;
        try
        {
            var next = await audio.ReadAsync();
            if (closed) return;
            updating = true;
            UpdateDevices(OutputDevice, next.Outputs, next.RenderId, "SMSL", current?.Outputs);
            UpdateDevices(InputDevice, next.Inputs, next.CaptureId, "In 1-2", current?.Inputs);
            bool installed = File.Exists(BackupPath);
            OutputDevice.IsEnabled = InputDevice.IsEnabled = !installed && !busy;
            DeviceHint.Visibility = installed ? Visibility.Visible : Visibility.Collapsed;
            string recoveryStatus = Path.Combine(Path.GetDirectoryName(BackupPath)!, "device-recovery-status.txt");
            string recovery = File.Exists(recoveryStatus) ? await File.ReadAllTextAsync(recoveryStatus) : "";
            DeviceHint.Text = recovery.StartsWith("Multiple matching")
                ? "같은 하드웨어의 출력이 여러 개입니다. 자동 연결을 보류했습니다. 사용할 장치만 연결해 주세요."
                : recovery.StartsWith("Device recovery needs attention")
                ? "장치 자동 복구를 완료하지 못했습니다. 처리 상태의 진단 기록을 확인해 주세요."
                : "출력 장치 ID가 바뀌면 같은 하드웨어를 자동으로 찾습니다. 확인에는 약 1분이 걸리며 복구 시 오디오가 잠시 끊길 수 있습니다.";
            EqSwitch.IsOn = next.EqEnabled;
            AecSwitch.IsOn = next.AecEnabled;
            EqSwitch.IsEnabled = AecSwitch.IsEnabled = ImportLeft.IsEnabled = ImportRight.IsEnabled = next.Ready && installed && !busy;
            ToolTipService.SetToolTip(EqSwitch, next.Ready ? "Windows 공유 모드의 출력에 적용됩니다." : "설치 및 적용을 완료하면 사용할 수 있습니다.");
            ToolTipService.SetToolTip(AecSwitch, next.Ready ? "MOTU 입력 1의 스피커 반향을 줄입니다." : "설치 및 적용을 완료하면 사용할 수 있습니다.");
            UpdateProfile(LeftSummary, LeftFilters, next.Left, current?.Left);
            UpdateProfile(RightSummary, RightFilters, next.Right, current?.Right);
            FilterHint.Text = next.Ready ? "필터는 이 PC에 저장됩니다. 원본 파일을 이동해도 설정이 유지됩니다."
                : "초기 필터: E:\\Speaker\\L.txt · R.txt. 설치 후 다른 파일로 변경할 수 있습니다.";
            SetupInfo.IsOpen = !installed;
            LocalInstallOptions.Visibility = installed ? Visibility.Collapsed : Visibility.Visible;
            LocalUnsignedOption.IsEnabled = !busy;
            SetupInfo.Title = "오디오 효과를 설정해 주세요";
            SetupInfo.Message = next.Left.Valid && next.Right.Valid
                ? "초기 필터가 준비되었습니다. 설치 및 적용에서 서명과 장치를 확인합니다."
                : "초기 REW 필터 파일이 필요합니다. E:\\Speaker 폴더에 L.txt와 R.txt를 준비해 주세요.";
            InstallButton.IsEnabled = !busy;
            RestoreButton.IsEnabled = installed && !busy;
            RenderStatus.Text = "스피커 EQ  ·  " + Describe(next.Render, installed);
            CaptureStatus.Text = "마이크 반향 제거  ·  " + Describe(next.Capture, installed);
            ReferenceStatus.Text = next.Reference ? "스피커 참조  ·  처리 활성" : "스피커 참조  ·  대기 / 학습 / 참조 없음";
            ClippingWarning.IsOpen = next.Clipping;
            FooterStatus.Text = !installed ? "설치 전 · 오디오 효과가 적용되지 않았습니다"
                : next.Render.Active || next.Capture.Active ? "오디오 처리 연결됨 · 자세한 내용은 처리 상태에서 확인"
                : "설치됨 · 실제 오디오 처리 확인 대기";
            Prerequisites.Text = $"왼쪽 필터: {(next.Left.Valid ? "준비됨" : "파일 필요")}\n오른쪽 필터: {(next.Right.Valid ? "준비됨" : "파일 필요")}\n설치: {(installed ? "등록됨 · 실제 처리는 위 상태에서 확인" : "미설치")}";
            current = next;
        }
        catch (Exception e)
        {
            if (!closed)
            {
                ShowMessage("오디오 상태를 읽지 못했습니다", e.Message, InfoBarSeverity.Error);
                FooterStatus.Text = "오디오 서비스 연결 오류";
                EqSwitch.IsEnabled = AecSwitch.IsEnabled = ImportLeft.IsEnabled = ImportRight.IsEnabled = false;
            }
        }
        finally { updating = false; refreshing = false; }
    }
    private static string Describe(Meter value, bool installed) => !installed ? "설치 전"
        : value.State + (value.Active ? $" · {value.Rate:N0} Hz" : "") + (value.Error != 0 ? $" · 오류 {value.Error}" : "");
    private static void UpdateProfile(TextBlock summary, ItemsControl filters, Profile next, Profile? previous)
    {
        if (previous is not null && next.Valid == previous.Valid && next.Summary == previous.Summary &&
            next.Filters.SequenceEqual(previous.Filters)) return;
        summary.Text = next.Summary;
        filters.ItemsSource = next.Filters;
    }
    private static void UpdateDevices(ComboBox combo, AudioDevice[] devices, string configured, string preferred, AudioDevice[]? previous)
    {
        string? selected = (combo.SelectedItem as AudioDevice)?.Id;
        if (previous is not null && devices.SequenceEqual(previous) &&
            (string.IsNullOrEmpty(configured) || selected == configured)) return;
        var list = devices.ToList();
        string target = !string.IsNullOrEmpty(configured) ? configured : selected ?? "";
        if (!string.IsNullOrEmpty(target) && list.All(d => d.Id != target))
            list.Add(new AudioDevice(target, "선택 장치 연결 대기"));
        combo.ItemsSource = list;
        combo.SelectedItem = list.FirstOrDefault(d => d.Id == target)
            ?? list.FirstOrDefault(d => d.Name.Contains(preferred, StringComparison.OrdinalIgnoreCase));
    }
    private async void ToggleEffect(object sender, RoutedEventArgs e)
    {
        if (updating || busy || current?.Ready != true || audio is null) return;
        bool eq = EqSwitch.IsOn, aec = AecSwitch.IsOn;
        busy = true;
        EqSwitch.IsEnabled = AecSwitch.IsEnabled = false;
        try { await audio.EnableAsync(eq, aec); }
        catch (Exception error) { ShowMessage("설정을 저장하지 못했습니다", error.Message, InfoBarSeverity.Error); }
        finally { busy = false; await RefreshAsync(); }
    }
    private async void ImportFilter(object sender, RoutedEventArgs e)
    {
        if (busy || current?.Ready != true || audio is null) return;
        busy = true;
        try
        {
            var picker = new FileOpenPicker();
            picker.FileTypeFilter.Add(".txt");
            InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));
            var file = await picker.PickSingleFileAsync();
            if (file is null) return;
            int channel = int.Parse((string)((Button)sender).Tag);
            await audio.ImportAsync(channel, file.Path);
            ShowMessage("필터를 가져왔습니다", channel == 0 ? "왼쪽 스피커에 적용했습니다." : "오른쪽 스피커에 적용했습니다.", InfoBarSeverity.Success);
        }
        catch (Exception error) { ShowMessage("필터를 가져오지 못했습니다", error.Message, InfoBarSeverity.Error); }
        finally { busy = false; await RefreshAsync(); }
    }
    private void ShowMessage(string title, string message, InfoBarSeverity severity)
    {
        MessageBar.Title = title; MessageBar.Message = message; MessageBar.Severity = severity; MessageBar.IsOpen = true;
    }
    private async Task ExplainAsync(string title, string message)
    {
        var dialog = new ContentDialog { XamlRoot = Root.XamlRoot, Title = title, Content = message,
            CloseButtonText = "확인", DefaultButton = ContentDialogButton.Close };
        await dialog.ShowAsync();
    }
    private async void Install(object sender, RoutedEventArgs e)
    {
        if (busy) return;
        if (OutputDevice.SelectedItem is not AudioDevice output || InputDevice.SelectedItem is not AudioDevice input)
        { await ExplainAsync("오디오 장치를 선택해 주세요", "연결된 스피커 출력과 마이크 입력을 선택한 후 다시 시도해 주세요."); return; }
        if (current?.Left.Valid != true || current.Right.Valid != true)
        { await ExplainAsync("초기 EQ 파일이 필요합니다", "E:\\Speaker\\L.txt와 E:\\Speaker\\R.txt에 유효한 REW Configurable_PEQ 파일을 준비해 주세요. 현재 오디오 설정은 변경되지 않았습니다."); return; }
        await RunInstallerAsync(false, output.Id, input.Id);
    }
    private async void Restore(object sender, RoutedEventArgs e)
    {
        if (busy) return;
        var dialog = new ContentDialog { XamlRoot = Root.XamlRoot, Title = "원래 오디오 설정으로 복원할까요?",
            Content = "SoundControl 효과 등록을 제거하고 설치 전 장치 설정과 오디오 보호 설정을 복원합니다. 가져온 필터는 보존됩니다.",
            PrimaryButtonText = "복원", CloseButtonText = "취소", DefaultButton = ContentDialogButton.Close };
        if (await dialog.ShowAsync() == ContentDialogResult.Primary) await RunInstallerAsync(true, "", "");
    }
    private async Task RunInstallerAsync(bool remove, string render, string capture)
    {
        bool localUnsigned = !remove && LocalUnsignedOption.IsChecked == true;
        busy = true; await RefreshAsync();
        try
        {
            string script = Path.Combine(PackageDirectory, remove ? "uninstall-windows.ps1" : "install-windows.ps1");
            if (!File.Exists(script)) throw new FileNotFoundException("설치 스크립트가 없습니다. 전체 패키지에서 앱을 실행해 주세요.", script);
            // Preflight captures the actual signature/endpoint failure without elevation.
            if (!remove)
            {
                var check = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardOutput = true, RedirectStandardError = true };
                foreach (var arg in new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script,
                    "-RenderId", render, "-CaptureId", capture, "-CheckOnly" }) check.ArgumentList.Add(arg);
                if (localUnsigned) check.ArgumentList.Add("-LocalUnsigned");
                using var preflight = Process.Start(check)!;
                var stdout = preflight.StandardOutput.ReadToEndAsync();
                var stderr = preflight.StandardError.ReadToEndAsync();
                await preflight.WaitForExitAsync();
                await stdout;
                string error = await stderr;
                if (preflight.ExitCode != 0) throw new InvalidOperationException(error);
            }
            string arguments = $"-NoProfile -ExecutionPolicy Bypass -File \"{script}\"";
            if (!remove) arguments += $" -RenderId \"{render}\" -CaptureId \"{capture}\"";
            if (localUnsigned) arguments += " -LocalUnsigned";
            using var process = Process.Start(new ProcessStartInfo("powershell.exe", arguments)
                { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden })!;
            await process.WaitForExitAsync();
            if (process.ExitCode != 0) throw new InvalidOperationException($"설치 도구가 완료되지 않았습니다 (코드 {process.ExitCode}).");
            await ExplainAsync(remove ? "원래 설정으로 복원했습니다" : "효과 등록을 완료했습니다",
                "PC를 다시 시작하고 재생·통화 앱을 다시 열어 주세요. 실제 오디오 처리는 처리 상태에서 확인할 수 있습니다.");
        }
        catch (Exception e) { await ExplainAsync("설치 작업을 완료하지 못했습니다", e.Message); }
        finally { busy = false; await RefreshAsync(); }
    }
    private async void ShowDiagnostics(object sender, RoutedEventArgs e)
    {
        DiagnosticLog.Visibility = Visibility.Visible;
        try
        {
            string path = Path.Combine(DataDirectory, "events.txt");
            DiagnosticLog.Text = File.Exists(path) ? await File.ReadAllTextAsync(path)
                : "아직 진단 기록이 없습니다. 설치 후 오디오 처리 상태가 바뀌면 기록됩니다.";
            string recoveryPath = Path.Combine(Path.GetDirectoryName(BackupPath)!, "device-recovery-status.txt");
            if (File.Exists(recoveryPath)) DiagnosticLog.Text += "\n장치 자동 복구: " + await File.ReadAllTextAsync(recoveryPath);
        }
        catch (Exception error) { DiagnosticLog.Text = error.Message; }
    }
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(nint hwnd);
    [DllImport("dwmapi.dll")] private static extern int DwmSetWindowAttribute(nint hwnd, uint attribute, ref int value, int size);
}
