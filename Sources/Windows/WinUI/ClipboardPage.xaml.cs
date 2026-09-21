using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage.Pickers;
using Windows.System;
using WinRT.Interop;

namespace SoundControl;

public sealed partial class ClipboardPage : UserControl
{
    private ClipboardController? controller;
    private Window? owner;
    private CancellationTokenSource? search;
    private Task? searchTask;
    private CancellationTokenSource? filter;
    private string[]? resultIds;
    private bool closed;
    internal Func<ClipEntry, Task>? PasteRequested;
    internal Action? Dismiss;
    public ClipboardPage() { InitializeComponent(); }
    internal void Configure(ClipboardController value, Window window)
    {
        controller = value; owner = window;
        controller.Store.Changed += StoreChanged;
        controller.Status += SetStatus;
        Refresh();
    }
    private void StoreChanged() => DispatcherQueue.TryEnqueue(() => { if (!closed) Refresh(); });
    private void SetStatus(string message) => DispatcherQueue.TryEnqueue(() => { if (!closed) StatusText.Text = message; });
    internal void FocusSearch() => Query.Focus(FocusState.Programmatic);
    private async void Refresh()
    {
        if (controller is null || History is null) return;
        filter?.Cancel();
        using var filtering = new CancellationTokenSource(); filter = filtering;
        try
        {
        string? selected = (History.SelectedItem as ClipEntry)?.Id;
        string kind = (Kind.SelectedItem as ComboBoxItem)?.Tag as string ?? "";
        var source = resultIds is null ? await controller.Store.FindAsync(Query.Text, filtering.Token) : controller.Store.Entries;
        filtering.Token.ThrowIfCancellationRequested();
        var entries = source.Where(x => kind.Length == 0 || x.Kind == kind);
        if (resultIds is not null) entries = entries.Where(x => resultIds.Contains(x.Id)).OrderBy(x => Array.IndexOf(resultIds, x.Id));
        else entries = entries.OrderByDescending(x => x.Pinned).ThenByDescending(x => x.CreatedAt);
        var list = entries.ToArray(); History.ItemsSource = list;
        History.SelectedItem = list.FirstOrDefault(x => x.Id == selected) ?? list.FirstOrDefault();
        CountLabel.Text = $"{list.Length}개";
        PauseButton.Content = controller.Store.Settings.Paused ? "기록 재개" : "기록 일시정지";
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { SetStatus(error.Message); }
        finally { if (filter == filtering) filter = null; }
    }
    private void QueryChanged(object sender, TextChangedEventArgs e)
    {
        search?.Cancel(); resultIds = null;
        if (Answer is not null) Answer.Text = "";
        Refresh();
    }
    private void FilterChanged(object sender, SelectionChangedEventArgs e) => Refresh();
    private async void SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        PreviewImage.Source = null;
        if (History.SelectedItem is not ClipEntry item || controller is null) { PreviewText.Text = "표시할 기록이 없습니다."; return; }
        try
        {
            var path = controller.Store.PayloadPath(item, item.Attachments[0]);
            if (item.Kind == "Image") { PreviewImage.Source = new BitmapImage(new Uri(path)); PreviewText.Text = item.Detail; }
            else if (item.Kind == "Text")
            {
                using var reader = File.OpenText(path);
                var buffer = new char[16000];
                int count = await reader.ReadBlockAsync(buffer, 0, buffer.Length);
                if ((History.SelectedItem as ClipEntry)?.Id == item.Id) PreviewText.Text = new string(buffer, 0, count) + (reader.EndOfStream ? "" : "\n… 미리보기 생략 (복사 시 전체 내용 사용)");
            }
            else PreviewText.Text = string.Join("\n", item.Attachments.Select(x => $"{x.OriginalName} · {x.Length:N0} bytes"));
        }
        catch (Exception error) { SetStatus(error.Message); }
    }
    private async Task RunAction(Func<ClipEntry, Task> action)
    {
        if (History.SelectedItem is not ClipEntry entry) return;
        try { await action(entry); } catch (Exception error) { SetStatus(error.Message); }
    }
    private async void CopyClick(object sender, RoutedEventArgs e) => await RunAction(x => controller!.CopyAsync(x));
    private async void PasteClick(object sender, RoutedEventArgs e) => await PasteSelectedAsync();
    private async Task PasteSelectedAsync() => await RunAction(x => PasteRequested?.Invoke(x) ?? controller!.CopyAsync(x));
    private async void PinClick(object sender, RoutedEventArgs e) => await RunAction(x => controller!.Store.SetPinnedAsync(x, !x.Pinned));
    private async void DeleteClick(object sender, RoutedEventArgs e) => await RunAction(x => controller!.Store.DeleteAsync(x));
    private async void HistoryDoubleTapped(object sender, DoubleTappedRoutedEventArgs e) => await PasteSelectedAsync();
    private async void HistoryKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Enter) { e.Handled = true; await PasteSelectedAsync(); }
        if (e.Key == VirtualKey.Delete) { e.Handled = true; await RunAction(x => controller!.Store.DeleteAsync(x)); }
    }
    private void PageKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Escape) { e.Handled = true; search?.Cancel(); Dismiss?.Invoke(); }
        if (e.Key == VirtualKey.Down && Query.FocusState != FocusState.Unfocused) { History.Focus(FocusState.Programmatic); e.Handled = true; }
    }
    private async void PauseClick(object sender, RoutedEventArgs e)
    {
        if (controller is null) return;
        try { await controller.UpdateSettingsAsync(controller.Store.Settings with { Paused = !controller.Store.Settings.Paused }); }
        catch (Exception error) { SetStatus(error.Message); }
    }
    private async void SearchClick(object sender, RoutedEventArgs e)
    {
        if (controller is null || search is not null) return;
        searchTask = SearchAsync(); await searchTask;
    }
    private async Task SearchAsync()
    {
        using var cancellation = new CancellationTokenSource(); search = cancellation;
        SearchButton.IsEnabled = false; CancelButton.Visibility = Visibility.Visible; Searching.IsActive = true;
        Answer.Text = ""; StatusText.Text = "Gemini 3.8 Flash High가 저장 폴더에서 검색 중입니다…";
        try
        {
            var result = await controller!.Search.SearchClipboardAsync(Query.Text, cancellation.Token);
            cancellation.Token.ThrowIfCancellationRequested();
            resultIds = result.ItemIds;
            Answer.Text = result.Answer + (result.Limitations.Length == 0 ? "" : "\n\n" + string.Join("\n", result.Limitations));
            Refresh(); StatusText.Text = "AI 검색 완료 · 결과를 선택해 복사하거나 붙여넣으세요.";
        }
        catch (OperationCanceledException) { StatusText.Text = "검색을 취소했습니다."; }
        catch (Exception error) { StatusText.Text = error.Message; }
        finally { search = null; SearchButton.IsEnabled = true; CancelButton.Visibility = Visibility.Collapsed; Searching.IsActive = false; }
    }
    private void CancelClick(object sender, RoutedEventArgs e) => search?.Cancel();
    private async void SettingsClick(object sender, RoutedEventArgs e)
    {
        if (controller is null || owner is null) return;
        var settings = controller.Store.Settings;
        var path = new TextBox { Header = "저장 폴더 (변경 시 빈 폴더 선택)", Text = settings.ArchivePath, IsReadOnly = true, TextWrapping = TextWrapping.Wrap };
        var browse = new Button { Content = "폴더 선택" };
        var excluded = new TextBox { Header = "기록 제외 앱 (.exe 이름, 쉼표 구분)", Text = string.Join(", ", settings.ExcludedApps) };
        var hotkey = new TextBox { Header = "전역 단축키", Text = FormatHotkey(settings) };
        var itemLimit = new NumberBox { Header = "항목당 한도 (MiB)", Value = settings.ItemLimitBytes / 1048576d, Minimum = 1, Maximum = 1024 };
        var totalLimit = new NumberBox { Header = "전체 한도 (GiB)", Value = settings.TotalLimitBytes / 1073741824d, Minimum = 1, Maximum = 1024 };
        var errorText = new TextBlock { TextWrapping = TextWrapping.Wrap };
        var panel = new StackPanel { Spacing = 12 };
        foreach (var control in new UIElement[] { path, browse, excluded, hotkey, itemLimit, totalLimit, errorText }) panel.Children.Add(control);
        var dialog = new ContentDialog { Title = "클립보드 설정", Content = new ScrollViewer { Content = panel }, PrimaryButtonText = "저장", CloseButtonText = "취소", XamlRoot = XamlRoot };
        browse.Click += async (_, _) =>
        {
            try
            {
                var picker = new FolderPicker(); picker.FileTypeFilter.Add("*");
                InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(owner));
                var folder = await picker.PickSingleFolderAsync(); if (folder is not null) path.Text = folder.Path;
            }
            catch (Exception error) { errorText.Text = error.Message; }
        };
        dialog.PrimaryButtonClick += async (_, args) =>
        {
            var deferral = args.GetDeferral(); dialog.IsPrimaryButtonEnabled = false;
            try
            {
                search?.Cancel(); if (searchTask is not null) await searchTask;
                var (modifiers, key) = ParseHotkey(hotkey.Text);
                if (!double.IsFinite(itemLimit.Value) || !double.IsFinite(totalLimit.Value)) throw new ArgumentException("저장 한도를 입력해 주세요.");
                await controller.UpdateSettingsAsync(settings with { ArchivePath = path.Text, ExcludedApps = excluded.Text.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries),
                    HotkeyModifiers = modifiers, HotkeyKey = key, ItemLimitBytes = checked((long)(itemLimit.Value * 1048576)), TotalLimitBytes = checked((long)(totalLimit.Value * 1073741824)) });
                SetStatus("설정을 저장했습니다. 이전 저장 폴더의 사본은 유지됩니다.");
            }
            catch (Exception error) { args.Cancel = true; errorText.Text = error.Message; }
            finally { dialog.IsPrimaryButtonEnabled = true; deferral.Complete(); }
        };
        await dialog.ShowAsync();
    }
    private static string FormatHotkey(ClipboardSettings s) => ((s.HotkeyModifiers & 2) != 0 ? "Ctrl+" : "") + ((s.HotkeyModifiers & 4) != 0 ? "Shift+" : "") + ((s.HotkeyModifiers & 1) != 0 ? "Alt+" : "") + (char)s.HotkeyKey;
    private static (uint, uint) ParseHotkey(string text)
    {
        var parts = text.ToUpperInvariant().Split('+', StringSplitOptions.TrimEntries);
        uint modifiers = 0;
        foreach (string part in parts[..^1]) modifiers |= part switch { "CTRL" => 2u, "SHIFT" => 4u, "ALT" => 1u, _ => throw new ArgumentException("Ctrl, Shift, Alt와 영문자·숫자를 조합해 주세요.") };
        string key = parts[^1];
        if (modifiers == 0 || key.Length != 1 || !(key[0] is >= 'A' and <= 'Z' or >= '0' and <= '9')) throw new ArgumentException("예: Ctrl+Shift+V 또는 Alt+V");
        return (modifiers, key[0]);
    }
    internal async Task ShutdownAsync()
    {
        closed = true; search?.Cancel(); filter?.Cancel();
        if (searchTask is not null) await searchTask;
        if (controller is not null) { controller.Store.Changed -= StoreChanged; controller.Status -= SetStatus; }
    }
}
