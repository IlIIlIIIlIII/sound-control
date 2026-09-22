using Microsoft.UI.Dispatching;
using Windows.ApplicationModel.DataTransfer;
using Windows.Graphics.Imaging;
using Windows.Storage;
using Windows.Storage.Streams;

namespace SoundControl;

internal sealed class ClipboardController : IAsyncDisposable
{
    public const string OwnFormat = "PersonalTools.Clipboard.Origin";
    private readonly DispatcherQueue dispatcher;
    private readonly CancellationTokenSource lifetime = new();
    private readonly HashSet<Task> captures = [];
    private ClipboardNative? native;
    private uint generation;
    public ClipboardStore Store { get; }
    public ClipboardSearch Search { get; }
    public event Action<string>? Status;
    public event Action? TogglePopup;
    public ClipboardController(AntigravityBridge bridge, DispatcherQueue dispatcher)
    {
        this.dispatcher = dispatcher;
        ClipboardSettings settings;
        try { settings = ClipboardSettings.Load(); }
        catch { settings = new() { Paused = true }; }
        Store = new(settings);
        Search = new(bridge, Store, BridgeSettings.Load);
        Store.Warning += message => dispatcher.TryEnqueue(() => Status?.Invoke(message));
    }
    public async Task StartAsync(nint window)
    {
        try
        {
            await Task.Run(() => Store.LoadAsync(lifetime.Token));
            if (lifetime.IsCancellationRequested) return;
            native = new(window, OnChanged, () => TogglePopup?.Invoke());
            native.SetHotkey(Store.Settings.HotkeyModifiers, Store.Settings.HotkeyKey);
            Status?.Invoke(Store.Settings.Paused ? "기록 일시정지" : "복사하는 내용을 자동으로 보관합니다.");
        }
        catch (Exception e) { Status?.Invoke(e.Message); }
    }
    private void OnChanged()
    {
        if (lifetime.IsCancellationRequested || Store.Settings.Paused) return;
        var source = ClipboardNative.SourceApp();
        if (Store.Settings.ExcludedApps.Any(x => Path.GetFileNameWithoutExtension(x).Equals(Path.GetFileNameWithoutExtension(source), StringComparison.OrdinalIgnoreCase))) return;
        uint current = ++generation;
        var task = CaptureAsync(current, source, ClipboardNative.GetClipboardSequenceNumber(), ClipboardNative.SourcePath());
        captures.Add(task);
        _ = task.ContinueWith(_ => dispatcher.TryEnqueue(() => captures.Remove(task)), TaskScheduler.Default);
    }
    private async Task CaptureAsync(uint current, string source, uint sequence, string? sourcePath)
    {
        try
        {
            await Task.Delay(100, lifetime.Token);
            for (int attempt = 0; attempt < 4; attempt++)
            {
                if (current != generation || sequence != ClipboardNative.GetClipboardSequenceNumber() || Store.Settings.Paused) return;
                try
                {
                    var data = Clipboard.GetContent();
                    if (data.Contains(OwnFormat) || data.Contains("ExcludeClipboardContentFromMonitorProcessing")) return;
                    ClipCapture? capture = null;
                    if (data.Contains(StandardDataFormats.StorageItems))
                    {
                        var items = await data.GetStorageItemsAsync();
                        if (items.Any(x => x is not StorageFile)) throw new IOException("폴더 복사는 보관하지 않습니다. 파일을 선택해 주세요.");
                        capture = new("Files", source, Files: items.Select(x => x.Path).ToArray());
                    }
                    else if (data.Contains(StandardDataFormats.Bitmap))
                    {
                        using var input = await (await data.GetBitmapAsync()).OpenReadAsync();
                        var decoder = await BitmapDecoder.CreateAsync(input);
                        if ((long)decoder.PixelWidth * decoder.PixelHeight * 4 > Store.Settings.ItemLimitBytes) throw new IOException("이미지 저장 한도를 초과했습니다.");
                        using var bitmap = await decoder.GetSoftwareBitmapAsync();
                        using var output = new InMemoryRandomAccessStream();
                        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, output);
                        encoder.SetSoftwareBitmap(bitmap); await encoder.FlushAsync();
                        if (output.Size > (ulong)Store.Settings.ItemLimitBytes) throw new IOException("이미지 저장 한도를 초과했습니다.");
                        output.Seek(0);
                        using var reader = new DataReader(output);
                        await reader.LoadAsync(checked((uint)output.Size));
                        var bytes = new byte[checked((int)output.Size)]; reader.ReadBytes(bytes);
                        capture = new("Image", source, Image: bytes);
                    }
                    else if (data.Contains(StandardDataFormats.Text)) capture = new("Text", source, Text: await data.GetTextAsync());
                    else if (data.Contains(StandardDataFormats.WebLink)) capture = new("Text", source, Text: (await data.GetWebLinkAsync()).AbsoluteUri);
                    if (capture is null || current != generation || sequence != ClipboardNative.GetClipboardSequenceNumber()) return;
                    capture = capture with { SourcePath = sourcePath };
                    await Task.Run(() => Store.SaveAsync(capture, lifetime.Token));
                    return;
                }
                catch (System.Runtime.InteropServices.COMException) when (attempt < 3) { await Task.Delay(75, lifetime.Token); }
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception e) { Status?.Invoke("클립보드 저장 실패: " + e.Message); }
    }
    public async Task CopyAsync(ClipEntry item)
    {
        var data = new DataPackage { RequestedOperation = DataPackageOperation.Copy };
        data.SetData(OwnFormat, "1");
        if (item.Kind == "Text") data.SetText(await File.ReadAllTextAsync(Store.PayloadPath(item, item.Attachments[0])));
        else if (item.Kind == "Image")
        {
            var file = await StorageFile.GetFileFromPathAsync(Store.PayloadPath(item, item.Attachments[0]));
            data.SetBitmap(RandomAccessStreamReference.CreateFromFile(file));
        }
        else
        {
            var files = new List<IStorageItem>();
            foreach (var attachment in item.Attachments) files.Add(await StorageFile.GetFileFromPathAsync(Store.PayloadPath(item, attachment)));
            data.SetStorageItems(files, true);
        }
        Clipboard.SetContent(data); Clipboard.Flush();
        Status?.Invoke("클립보드에 복사했습니다.");
    }
    public async Task UpdateSettingsAsync(ClipboardSettings settings)
    {
        var old = Store.Settings;
        // Register first so a conflict cannot silently persist an unusable shortcut.
        try
        {
            native?.SetHotkey(settings.HotkeyModifiers, settings.HotkeyKey);
            await Task.Run(() => Store.UpdateSettingsAsync(settings, token: lifetime.Token));
        }
        catch
        {
            try { native?.SetHotkey(old.HotkeyModifiers, old.HotkeyKey); } catch { }
            throw;
        }
    }
    public async ValueTask DisposeAsync()
    {
        lifetime.Cancel(); native?.Dispose(); native = null;
        await Task.WhenAll(captures.ToArray());
        lifetime.Dispose();
    }
}
