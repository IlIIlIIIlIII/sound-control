using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Diagnostics;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json;
using Windows.ApplicationModel.DataTransfer;

namespace PersonalTools;

public sealed partial class AntigravityPage : UserControl
{
    private AntigravityBridge bridge => ((App)Application.Current).Bridge;
    private readonly CancellationTokenSource lifetime = new();
    private BridgeSettings? running;
    private bool busy;
    public AntigravityPage()
    {
        InitializeComponent();
        bridge.Status += message => DispatcherQueue.TryEnqueue(() => BridgeInfo.Title = message);
        KeyBox.Password = bridge.ApiKey;
        // PasswordBox cannot be read-only: keep the actual connection key authoritative.
        KeyBox.PasswordChanged += (_, _) => { if (KeyBox.Password != bridge.ApiKey) KeyBox.Password = bridge.ApiKey; };
        BridgeSettings settings;
        try { settings = BridgeSettings.Load(); }
        catch (Exception e) { settings = new(BridgeSettings.DefaultCli); ShowError(e); }
        CliPath.Text = settings.CliPath;
        PortBox.Value = settings.Port;
        TimeoutBox.Value = settings.TimeoutSeconds;
        ModelBox.ItemsSource = new[] { settings.Model };
        ModelBox.Text = settings.Model;
    }
    private BridgeSettings Settings()
    {
        if (!double.IsFinite(PortBox.Value) || !double.IsFinite(TimeoutBox.Value) || PortBox.Value != Math.Truncate(PortBox.Value) || TimeoutBox.Value != Math.Truncate(TimeoutBox.Value))
            throw new ArgumentException("포트와 제한 시간은 정수로 입력해 주세요.");
        var settings = new BridgeSettings(CliPath.Text.Trim(), checked((int)PortBox.Value), ModelBox.Text.Trim(), checked((int)TimeoutBox.Value));
        settings.Validate(); return settings;
    }
    private void ShowError(Exception error)
    {
        BridgeInfo.Severity = InfoBarSeverity.Error;
        BridgeInfo.Title = "연결 확인이 필요합니다";
        BridgeInfo.Message = error is OperationCanceledException ? "작업 시간이 초과되었거나 취소되었습니다." : error.Message;
    }
    private void UpdateControls()
    {
        bool editable = !busy && !bridge.IsRunning;
        CliPath.IsEnabled = PortBox.IsEnabled = TimeoutBox.IsEnabled = ModelBox.IsEnabled = StartButton.IsEnabled = editable;
        CheckButton.IsEnabled = LoginButton.IsEnabled = !busy;
        StopButton.IsEnabled = TestButton.IsEnabled = !busy && bridge.IsRunning;
    }
    private async void CheckCli(object sender, RoutedEventArgs e)
    {
        busy = true; UpdateControls();
        try
        {
            var settings = Settings();
            var models = await AntigravityBridge.GetModelsAsync(settings, lifetime.Token);
            ModelBox.ItemsSource = models;
            ModelBox.Text = settings.Model;
            BridgeInfo.Severity = InfoBarSeverity.Success;
            BridgeInfo.Title = "CLI 연결 확인됨";
            BridgeInfo.Message = $"모델 {models.Length - 1}개를 확인했습니다.";
        }
        catch (Exception error) { ShowError(error); }
        finally { busy = false; UpdateControls(); }
    }
    private void OpenCli(object sender, RoutedEventArgs e)
    {
        try
        {
            var settings = Settings();
            Directory.CreateDirectory(BridgeSettings.DirectoryPath);
            Process.Start(new ProcessStartInfo(settings.CliPath) { UseShellExecute = true, WorkingDirectory = BridgeSettings.DirectoryPath });
        }
        catch (Exception error) { ShowError(error); }
    }
    private async void StartBridge(object sender, RoutedEventArgs e) => await StartBridgeAsync();
    public async Task StartBridgeAsync()
    {
        if (busy || bridge.IsRunning || lifetime.IsCancellationRequested) return;
        busy = true; UpdateControls();
        try
        {
            var settings = Settings();
            settings.Save();
            await bridge.StartAsync(settings, lifetime.Token);
            running = settings;
            BaseUrl.Text = $"http://127.0.0.1:{settings.Port}/v1";
            BridgeInfo.Severity = InfoBarSeverity.Success;
            BridgeInfo.Message = "연결할 앱에 Base URL과 API 키를 입력해 주세요.";
        }
        catch (Exception error) { ShowError(error); }
        finally { busy = false; UpdateControls(); }
    }
    private async void StopBridge(object sender, RoutedEventArgs e)
    {
        busy = true; UpdateControls();
        try { await bridge.StopAsync(); running = null; BridgeInfo.Severity = InfoBarSeverity.Informational; BridgeInfo.Message = "새 요청을 받지 않습니다."; }
        catch (Exception error) { ShowError(error); }
        finally { busy = false; UpdateControls(); }
    }
    private void CopyConnection(object sender, RoutedEventArgs e)
    {
        try
        {
            var data = new DataPackage();
            int port = running?.Port ?? Settings().Port;
            data.SetData(ClipboardController.OwnFormat, "1");
            data.SetText($"OPENAI_BASE_URL=http://127.0.0.1:{port}/v1\nOPENAI_API_KEY={bridge.ApiKey}");
            Clipboard.SetContent(data);
            BridgeInfo.Message = "연결 주소와 키를 복사했습니다.";
        }
        catch (Exception error) { ShowError(error); }
    }
    private async void TestBridge(object sender, RoutedEventArgs e)
    {
        if (running is null) return;
        TestButton.IsEnabled = false;
        try
        {
            using var client = new HttpClient { Timeout = TimeSpan.FromSeconds(running.TimeoutSeconds + 5) };
            client.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", bridge.ApiKey);
            using var response = await client.PostAsJsonAsync($"http://127.0.0.1:{running.Port}/v1/chat/completions",
                new { model = running.Model, messages = new[] { new { role = "user", content = TestPrompt.Text } } }, lifetime.Token);
            using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync(lifetime.Token));
            TestResult.Text = response.IsSuccessStatusCode ? json.RootElement.GetProperty("choices")[0].GetProperty("message").GetProperty("content").GetString()
                : json.RootElement.GetProperty("error").GetProperty("message").GetString();
        }
        catch (Exception error) { ShowError(error); }
        finally { UpdateControls(); }
    }
    public async Task ShutdownAsync() { lifetime.Cancel(); await bridge.StopAsync(); }
}
