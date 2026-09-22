using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage.Pickers;
using Windows.System;
using WinRT.Interop;

namespace PersonalTools;

public sealed record ClipboardCard(ClipEntry Entry, string Format)
{
    public override string ToString() => $"{FormatLabel}, {Entry.Preview}, {Entry.SourceApp}, {CopiedAt}";
    public string FormatLabel => (Entry.Pinned ? "★ " : "") + (Format switch { "Url" => "URL", "Image" => "이미지", "Files" => "파일", _ => "텍스트" });
    public string FormatIcon => Format switch { "Url" => "\uE71B", "Image" => "\uE91B", "Files" => "\uE8B7", _ => "\uE8A5" };
    public string CopiedAt => Entry.CreatedAt.ToLocalTime().ToString("yyyy-MM-dd HH:mm");
    public string AppLabel => Path.GetFileNameWithoutExtension(Entry.SourceApp);
    public Visibility TextVisibility => Entry.Kind == "Image" ? Visibility.Collapsed : Visibility.Visible;
    public Visibility ImageVisibility => Entry.Kind == "Image" ? Visibility.Visible : Visibility.Collapsed;
}

public sealed partial class ClipboardPage : UserControl
{
    private ClipboardController? controller;
    private Window? owner;
    private CancellationTokenSource? search, filter;
    private Task? searchTask;
    private string[]? resultIds;
    private HashSet<string>? searchScope;
    private bool closed, active = true, updatingTabs;
    private int searchRevision;
    private ClipboardFilter filters = new();
    private string selectedCategory = "";
    private string categorySignature = "";
    private ContentDialog? currentDialog;
    private readonly HashSet<FlyoutBase> openFlyouts = [];
    internal bool OwnedPickerOpen { get; private set; }
    internal Func<ClipEntry, Task>? PasteRequested;
    internal Action? Dismiss;
    private readonly Windows.UI.ViewManagement.UISettings appearance = new();
    private bool desktopAcrylic;
    public ClipboardPage()
    {
        InitializeComponent();
        History.AddHandler(UIElement.PointerWheelChangedEvent, new PointerEventHandler(HistoryWheelChanged), true);
        Loaded += (_, _) => appearance.ColorValuesChanged += AppearanceChanged;
        Unloaded += (_, _) => appearance.ColorValuesChanged -= AppearanceChanged;
    }
    private void AppearanceChanged(Windows.UI.ViewManagement.UISettings sender, object args) => DispatcherQueue.TryEnqueue(() => { if (desktopAcrylic) UpdatePanelTint(); Refresh(); });
    internal void UseDesktopAcrylic()
    {
        // Keep the XAML layer clear so Window.SystemBackdrop can render its material
        // and its automatic transparency-off / high-contrast / battery-saver fallback.
        RequestedTheme = ElementTheme.Default;
        desktopAcrylic = true;
        UpdatePanelTint();
    }
    private void UpdatePanelTint() => PanelChrome.Background = new SolidColorBrush(appearance.GetColorValue(Windows.UI.ViewManagement.UIColorType.Accent))
    { Opacity = new Windows.UI.ViewManagement.AccessibilitySettings().HighContrast ? 0 : 0.035 };
    private bool IsAi => AiMode?.IsOn == true;
    private ClipboardFilter Scope => new() { CategoryId = selectedCategory is "" or "*" ? null : selectedCategory, PinnedOnly = selectedCategory == "*" };
    private ClipEntry? Selected => (History.SelectedItem as ClipboardCard)?.Entry;
    internal void Configure(ClipboardController value, Window window)
    {
        controller = value; owner = window;
        controller.Store.Changed += StoreChanged;
        controller.Status += SetStatus;
        Refresh();
    }
    private void StoreChanged() => DispatcherQueue.TryEnqueue(() =>
    {
        if (closed || !active || controller is null) return;
        if (searchScope is not null && !searchScope.SetEquals(CurrentScopeIds()))
        {
            CancelSearch(); StatusText.Text = "검색 범위가 변경되었습니다. 다시 검색해 주세요.";
        }
        Refresh();
    });
    private string[] CurrentScopeIds()
    {
        var scope = Scope;
        return controller!.Store.Entries.Where(x => scope.MatchesMetadata(x) && controller.Store.InCategory(x, scope.CategoryId)).Select(x => x.Id).ToArray();
    }
    private void SetStatus(string message) => DispatcherQueue.TryEnqueue(() => { if (!closed && active) StatusText.Text = message; });
    internal void FocusSearch() { active = true; Refresh(); Query.Focus(FocusState.Programmatic); }
    internal void Suspend()
    {
        active = false; CancelSearch(); filter?.Cancel(); currentDialog?.Hide();
        foreach (var flyout in openFlyouts.ToArray()) flyout.Hide();
    }
    private void CancelSearch()
    {
        ++searchRevision; search?.Cancel(); resultIds = null; searchScope = null;
        if (Answer is null) return;
        Answer.Text = ""; AnswerContainer.Visibility = Visibility.Collapsed;
        StatusText.Text = "";
        Searching.Visibility = CancelButton.Visibility = Visibility.Collapsed;
        SearchButton.IsEnabled = true;
    }
    private void UpdateTabs()
    {
        if (controller is null) return;
        string signature = string.Join("|", controller.Store.Categories.Select(x => x.Id + ":" + x.Name));
        if (signature == categorySignature && CategoryTabs.ItemsSource is not null) return;
        categorySignature = signature;
        if (selectedCategory is not ("" or "*") && !controller.Store.Categories.Any(x => x.Id == selectedCategory)) { selectedCategory = ""; CancelSearch(); }
        updatingTabs = true;
        var tabs = new[] { new ClipCategory("", "전체"), new ClipCategory("*", "고정") }.Concat(controller.Store.Categories).ToArray();
        CategoryTabs.ItemsSource = tabs;
        CategoryTabs.SelectedItem = tabs.First(x => x.Id == selectedCategory);
        updatingTabs = false;
    }
    private async void Refresh(bool debounce = false)
    {
        if (controller is null || History is null || closed || !active) return;
        filter?.Cancel();
        using var filtering = new CancellationTokenSource(); filter = filtering;
        try
        {
            UpdateTabs();
            string? selected = Selected?.Id;
            var options = IsAi ? Scope : filters with { CategoryId = Scope.CategoryId, PinnedOnly = Scope.PinnedOnly };
            string query = IsAi ? "" : Query.Text;
            var ids = resultIds;
            if (debounce) await Task.Delay(200, filtering.Token);
            var found = await Task.Run(() => controller.Store.FindAsync(query, options, filtering.Token), filtering.Token);
            IEnumerable<ClipEntry> entries = ids is null ? found.OrderByDescending(x => x.Pinned).ThenByDescending(x => x.CreatedAt)
                : found.Where(x => ids.Contains(x.Id)).OrderBy(x => Array.IndexOf(ids, x.Id));
            var cards = await Task.Run(async () =>
            {
                var rows = new List<ClipboardCard>();
                foreach (var entry in entries)
                {
                    filtering.Token.ThrowIfCancellationRequested();
                    rows.Add(new(entry, await controller.Store.GetFormatAsync(entry, filtering.Token)));
                }
                return rows;
            }, filtering.Token);
            filtering.Token.ThrowIfCancellationRequested();
            if (!active || closed) return;
            History.ItemsSource = cards;
            History.SelectedItem = cards.FirstOrDefault(x => x.Entry.Id == selected) ?? cards.FirstOrDefault();
            EmptyLabel.Text = controller.Store.Entries.Count == 0 ? "복사한 기록이 없습니다." : "조건에 맞는 기록이 없습니다.";
            EmptyLabel.Visibility = cards.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            CountLabel.Text = $"{cards.Count}개";
            PauseButton.Label = controller.Store.Settings.Paused ? "기록 재개" : "기록 일시정지";
            PauseButton.Icon = new SymbolIcon(controller.Store.Settings.Paused ? Symbol.Play : Symbol.Pause);
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { SetStatus(error.Message); }
        finally { if (filter == filtering) filter = null; }
    }
    private void QueryChanged(object sender, TextChangedEventArgs e) { CancelSearch(); Refresh(true); }
    private void ModeChanged(object sender, RoutedEventArgs e)
    {
        if (Query is null || FilterButton is null) return;
        CancelSearch();
        Query.PlaceholderText = "기록 검색 · Enter 일반 / Shift+Enter AI";
        FilterButton.Visibility = IsAi ? Visibility.Collapsed : Visibility.Visible;
        SearchButton.Visibility = IsAi ? Visibility.Visible : Visibility.Collapsed;
        StatusText.Text = ""; Refresh();
    }
    private void CategoryChanged(object sender, SelectionChangedEventArgs e)
    {
        if (updatingTabs || CategoryTabs.SelectedItem is not ClipCategory category) return;
        selectedCategory = category.Id; CancelSearch(); Refresh();
    }
    private async Task RunAction(ClipEntry? entry, Func<ClipEntry, Task> action)
    {
        if (entry is null) return;
        try { await action(entry); } catch (Exception error) { SetStatus(error.Message); }
    }
    private Task PasteAsync(ClipEntry entry) => RunAction(entry, x => PasteRequested?.Invoke(x) ?? controller!.CopyAsync(x));
    private async void CardDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is ClipboardCard card) { e.Handled = true; await PasteAsync(card.Entry); }
    }
    private async void HistoryKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Enter && Selected is { } entry) { e.Handled = true; await PasteAsync(entry); }
        if (e.Key == VirtualKey.Delete) { e.Handled = true; await RunAction(Selected, x => controller!.Store.DeleteAsync(x)); }
    }
    private void FocusSearchInvoked(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
    {
        if (!active || closed || currentDialog is not null || OwnedPickerOpen) return;
        args.Handled = true;
        Query.Focus(FocusState.Keyboard);
        Query.SelectAll();
    }
    private void PageKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Escape) { e.Handled = true; CancelSearch(); Dismiss?.Invoke(); }
        if (e.Key == VirtualKey.Down && Query.FocusState != FocusState.Unfocused) { History.Focus(FocusState.Programmatic); e.Handled = true; }
    }
    private async void QueryKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != VirtualKey.Enter) return;
        e.Handled = true;
        bool ai = (Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift)
            & Windows.UI.Core.CoreVirtualKeyStates.Down) != 0;
        AiMode.IsOn = ai;
        if (ai) await BeginSearchAsync();
        else { CancelSearch(); Refresh(); }
    }
    private void HistoryWheelChanged(object sender, PointerRoutedEventArgs e)
    {
        var point = e.GetCurrentPoint(History).Properties;
        var scroller = FindScrollViewer(History);
        if (scroller is null || point.MouseWheelDelta == 0) return;
        double delta = point.IsHorizontalMouseWheel ? -point.MouseWheelDelta : point.MouseWheelDelta;
        scroller.ChangeView(Math.Clamp(scroller.HorizontalOffset + delta, 0, scroller.ScrollableWidth), null, null, true);
        e.Handled = true;
    }
    private static ScrollViewer? FindScrollViewer(DependencyObject root)
    {
        if (root is ScrollViewer viewer) return viewer;
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
            if (FindScrollViewer(VisualTreeHelper.GetChild(root, i)) is { } found) return found;
        return null;
    }
    private async void PauseClick(object sender, RoutedEventArgs e)
    {
        if (controller is null) return;
        try { await controller.UpdateSettingsAsync(controller.Store.Settings with { Paused = !controller.Store.Settings.Paused }); }
        catch (Exception error) { SetStatus(error.Message); }
    }
    private async void SearchClick(object sender, RoutedEventArgs e) => await BeginSearchAsync();
    private async Task BeginSearchAsync()
    {
        if (controller is null || !IsAi || !active) return;
        CancelSearch(); int revision = searchRevision;
        if (searchTask is not null) await searchTask;
        if (revision != searchRevision || !active) return;
        searchTask = SearchAsync(revision); await searchTask;
    }
    private async Task SearchAsync(int revision)
    {
        using var cancellation = new CancellationTokenSource(); search = cancellation;
        SearchButton.IsEnabled = false; CancelButton.Visibility = Searching.Visibility = Visibility.Visible;
        StatusText.Text = "선택한 범위에서 AI 검색 중…";
        try
        {
            var ids = selectedCategory.Length == 0 ? null : CurrentScopeIds();
            searchScope = ids?.ToHashSet(StringComparer.Ordinal);
            var result = await controller!.Search.SearchClipboardAsync(Query.Text, ids, cancellation.Token);
            cancellation.Token.ThrowIfCancellationRequested();
            if (revision != searchRevision || !active || !IsAi) return;
            resultIds = result.ItemIds;
            Answer.Text = result.Answer + (result.Limitations.Length == 0 ? "" : "\n" + string.Join("\n", result.Limitations));
            AnswerContainer.Visibility = Visibility.Visible;
            Refresh(); StatusText.Text = "AI 검색 완료 · 카드를 선택해 붙여넣으세요.";
        }
        catch (OperationCanceledException) { }
        catch (Exception error)
        {
            if (revision == searchRevision && active) { Answer.Text = error.Message + " 다시 검색해 주세요."; AnswerContainer.Visibility = Visibility.Visible; StatusText.Text = ""; }
        }
        finally
        {
            if (search == cancellation) search = null;
            if (revision == searchRevision) { SearchButton.IsEnabled = true; CancelButton.Visibility = Searching.Visibility = Visibility.Collapsed; }
        }
    }
    private void CancelClick(object sender, RoutedEventArgs e) { CancelSearch(); StatusText.Text = "검색을 취소했습니다."; Refresh(); }
    private void ShowFlyout(FlyoutBase flyout, FrameworkElement anchor)
    {
        openFlyouts.Add(flyout); flyout.Closed += (_, _) => openFlyouts.Remove(flyout);
        flyout.ShowAt(anchor);
    }
    private async Task<ContentDialogResult> ShowDialogAsync(ContentDialog dialog)
    {
        if (currentDialog is not null) return ContentDialogResult.None;
        currentDialog = dialog;
        try { return await dialog.ShowAsync(); }
        finally { currentDialog = null; }
    }
    private void CardContextRequested(UIElement sender, ContextRequestedEventArgs args)
    {
        if (sender is FrameworkElement element && element.DataContext is ClipboardCard card) { args.Handled = true; CardMenu(element, card.Entry); }
    }
    private void CardMenuClick(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement element && element.DataContext is ClipboardCard card) CardMenu(element, card.Entry);
    }
    private void CardMenu(FrameworkElement anchor, ClipEntry entry)
    {
        History.SelectedItem = ((IEnumerable<ClipboardCard>)History.ItemsSource).FirstOrDefault(x => x.Entry.Id == entry.Id);
        var menu = new MenuFlyout();
        void Add(string text, Func<ClipEntry, Task> action)
        {
            var item = new MenuFlyoutItem { Text = text }; item.Click += async (_, _) => await RunAction(entry, action); menu.Items.Add(item);
        }
        Add("붙여넣기", PasteAsync); Add("복사", x => controller!.CopyAsync(x)); Add("미리보기", PreviewAsync);
        Add("카테고리 지정", AssignCategoriesAsync);
        Add(entry.Pinned ? "고정 해제" : "고정", x => controller!.Store.SetPinnedAsync(x, !x.Pinned));
        menu.Items.Add(new MenuFlyoutSeparator()); Add("삭제", x => controller!.Store.DeleteAsync(x));
        ShowFlyout(menu, anchor);
    }
    private async Task PreviewAsync(ClipEntry entry)
    {
        if (controller is null) return;
        var panel = new StackPanel { Spacing = 8, MinWidth = 240 };
        if (entry.Kind == "Image") panel.Children.Add(new Image { Source = new BitmapImage(new Uri(controller.Store.PayloadPath(entry, entry.Attachments[0]))), MaxHeight = 400, Stretch = Stretch.Uniform });
        else
        {
            string text;
            if (entry.Kind == "Text")
            {
                using var reader = File.OpenText(controller.Store.PayloadPath(entry, entry.Attachments[0]));
                var buffer = new char[16000]; int count = await reader.ReadBlockAsync(buffer, 0, buffer.Length);
                text = new string(buffer, 0, count) + (reader.EndOfStream ? "" : "\n… 미리보기 생략 (복사 시 전체 내용 사용)");
            }
            else text = string.Join("\n", entry.Attachments.Select(x => $"{x.OriginalName} · {x.Length:N0} bytes"));
            panel.Children.Add(new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true });
        }
        if (!active) return;
        await ShowDialogAsync(new ContentDialog { Title = "미리보기", Content = new ScrollViewer { Content = panel, MaxHeight = 400 }, CloseButtonText = "닫기", XamlRoot = XamlRoot });
    }
    private async Task AssignCategoriesAsync(ClipEntry entry)
    {
        if (controller is null) return;
        var panel = new StackPanel { Spacing = 8 };
        var selected = controller.Store.CategoryIds(entry.Id);
        var checks = controller.Store.Categories.Select(x => new CheckBox { Content = x.Name, Tag = x.Id, IsChecked = selected.Contains(x.Id) }).ToArray();
        foreach (var check in checks) panel.Children.Add(check);
        if (checks.Length == 0) panel.Children.Add(new TextBlock { Text = "상단 + 버튼으로 카테고리를 먼저 만들어 주세요." });
        var dialog = new ContentDialog { Title = "카테고리 지정", Content = new ScrollViewer { Content = panel, MaxHeight = 300 }, PrimaryButtonText = "저장", CloseButtonText = "취소", XamlRoot = XamlRoot };
        if (await ShowDialogAsync(dialog) == ContentDialogResult.Primary)
            await controller.Store.SetCategoriesAsync(entry.Id, checks.Where(x => x.IsChecked == true).Select(x => (string)x.Tag));
    }
    private async void AddCategoryClick(object sender, RoutedEventArgs e) => await EditCategoryAsync(null);
    private void CategoryContextRequested(UIElement sender, ContextRequestedEventArgs args)
    {
        if (sender is not FrameworkElement element || element.DataContext is not ClipCategory category || category.Id is "" or "*") return;
        args.Handled = true;
        var menu = new MenuFlyout();
        var rename = new MenuFlyoutItem { Text = "이름 변경" }; rename.Click += async (_, _) => await EditCategoryAsync(category);
        var delete = new MenuFlyoutItem { Text = "카테고리 삭제 (기록 유지)" };
        delete.Click += async (_, _) => { try { await controller!.Store.DeleteCategoryAsync(category.Id); } catch (Exception error) { SetStatus(error.Message); } };
        menu.Items.Add(rename); menu.Items.Add(delete); ShowFlyout(menu, element);
    }
    private async Task EditCategoryAsync(ClipCategory? category)
    {
        var name = new TextBox { Text = category?.Name ?? "", PlaceholderText = "카테고리 이름", MaxLength = 60 };
        var error = new TextBlock { TextWrapping = TextWrapping.Wrap };
        var content = new StackPanel { Spacing = 8 }; content.Children.Add(name); content.Children.Add(error);
        var dialog = new ContentDialog { Title = category is null ? "카테고리 추가" : "카테고리 이름 변경", Content = content, PrimaryButtonText = "저장", CloseButtonText = "취소", DefaultButton = ContentDialogButton.Primary, XamlRoot = XamlRoot };
        dialog.PrimaryButtonClick += async (_, args) =>
        {
            var deferral = args.GetDeferral();
            try { await controller!.Store.SaveCategoryAsync(name.Text, category?.Id); }
            catch (Exception exception) { args.Cancel = true; error.Text = exception.Message; }
            finally { deferral.Complete(); }
        };
        try { await ShowDialogAsync(dialog); } catch (Exception exception) { SetStatus(exception.Message); }
    }
    private void FiltersClick(object sender, RoutedEventArgs e)
    {
        if (controller is null) return;
        var panel = new StackPanel { Spacing = 10, Width = 290 };
        panel.Children.Add(new TextBlock { Text = "복사한 프로세스", FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });
        var processes = controller.Store.Entries.Select(x => x.SourceApp).Concat(filters.Processes).Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(x => x).Select(x => new CheckBox { Content = x, IsChecked = filters.Processes.Contains(x, StringComparer.OrdinalIgnoreCase) }).ToArray();
        var apps = new StackPanel(); foreach (var check in processes) apps.Children.Add(check);
        panel.Children.Add(new ScrollViewer { Content = apps, MaxHeight = 120 });
        panel.Children.Add(new TextBlock { Text = "형식 · 선택하지 않으면 전체", FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });
        var kinds = new[] { ("Text", "텍스트"), ("Image", "이미지"), ("Url", "URL"), ("Files", "파일") }
            .Select(x => new CheckBox { Content = x.Item2, Tag = x.Item1, IsChecked = filters.Formats.Contains(x.Item1) }).ToArray();
        foreach (var check in kinds) panel.Children.Add(check);
        var preset = new ComboBox { Header = "복사한 날짜", ItemsSource = new[] { "전체", "오늘", "최근 7일", "최근 30일", "직접 지정" }, SelectedIndex = filters.From is null && filters.Through is null ? 0 : 4, HorizontalAlignment = HorizontalAlignment.Stretch };
        var from = new CalendarDatePicker { PlaceholderText = "시작일", Date = filters.From is { } start ? new DateTimeOffset(start.ToDateTime(TimeOnly.MinValue)) : null };
        var through = new CalendarDatePicker { PlaceholderText = "종료일", Date = filters.Through is { } end ? new DateTimeOffset(end.ToDateTime(TimeOnly.MinValue)) : null };
        from.Visibility = through.Visibility = preset.SelectedIndex == 4 ? Visibility.Visible : Visibility.Collapsed;
        preset.SelectionChanged += (_, _) => from.Visibility = through.Visibility = preset.SelectedIndex == 4 ? Visibility.Visible : Visibility.Collapsed;
        panel.Children.Add(preset); panel.Children.Add(from); panel.Children.Add(through);
        var error = new TextBlock { TextWrapping = TextWrapping.Wrap }; panel.Children.Add(error);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var apply = new Button { Content = "적용" }; var reset = new Button { Content = "초기화" };
        buttons.Children.Add(apply); buttons.Children.Add(reset); panel.Children.Add(buttons);
        var flyout = new Flyout { Content = new ScrollViewer { Content = panel, MaxHeight = Math.Max(160, Math.Min(500, XamlRoot.Size.Height - 80)) } };
        apply.Click += (_, _) =>
        {
            var today = DateOnly.FromDateTime(DateTime.Now);
            DateOnly? first = preset.SelectedIndex switch { 1 => today, 2 => today.AddDays(-6), 3 => today.AddDays(-29), 4 => from.Date is { } d ? DateOnly.FromDateTime(d.DateTime) : null, _ => null };
            DateOnly? last = preset.SelectedIndex is 1 or 2 or 3 ? today : preset.SelectedIndex == 4 && through.Date is { } endDate ? DateOnly.FromDateTime(endDate.DateTime) : null;
            if (first > last) { error.Text = "시작일은 종료일보다 늦을 수 없습니다."; return; }
            filters = new() { Processes = processes.Where(x => x.IsChecked == true).Select(x => (string)x.Content).ToArray(), Formats = kinds.Where(x => x.IsChecked == true).Select(x => (string)x.Tag).ToArray(), From = first, Through = last };
            FilterButton.Content = filters.Processes.Length + filters.Formats.Length > 0 || first is not null || last is not null ? "필터 · 적용됨" : "필터";
            flyout.Hide(); Refresh();
        };
        reset.Click += (_, _) => { filters = new(); FilterButton.Content = "필터"; flyout.Hide(); Refresh(); };
        ShowFlyout(flyout, FilterButton);
    }
    private void SurfaceSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (ToolsBar is null) return;
        bool compact = e.NewSize.Width < 850;
        CategoryTabs.MaxWidth = Math.Max(100, e.NewSize.Width - 220);
        SearchSpace.ColumnDefinitions[0].Width = new GridLength(compact ? Math.Max(180, e.NewSize.Width - 280) : 440);
        foreach (var button in ToolsBar.PrimaryCommands.OfType<AppBarButton>().ToArray()) if (compact) { ToolsBar.PrimaryCommands.Remove(button); ToolsBar.SecondaryCommands.Add(button); }
        foreach (var button in ToolsBar.SecondaryCommands.OfType<AppBarButton>().ToArray()) if (!compact) { ToolsBar.SecondaryCommands.Remove(button); ToolsBar.PrimaryCommands.Add(button); }
        ToolsBar.DefaultLabelPosition = compact ? CommandBarDefaultLabelPosition.Collapsed : CommandBarDefaultLabelPosition.Right;
    }
    private void CardHeaderLoaded(object sender, RoutedEventArgs e) => UpdateCardHeader((Border)sender);
    private void CardHeaderDataContextChanged(FrameworkElement sender, DataContextChangedEventArgs args) => UpdateCardHeader((Border)sender);
    private void CardHeaderThemeChanged(FrameworkElement sender, object args) => UpdateCardHeader((Border)sender);
    private void UpdateCardHeader(Border border)
    {
        if (border.DataContext is not ClipboardCard card) return;
        if (new Windows.UI.ViewManagement.AccessibilitySettings().HighContrast)
        {
            border.Background = (Brush)Application.Current.Resources["SystemControlBackgroundBaseLowBrush"];
            return;
        }
        // Semantic type colours sit on the card header only. The surface, text,
        // focus and selection continue to use the user's Windows theme/accent.
        bool dark = border.ActualTheme == ElementTheme.Dark;
        (int r, int g, int b) = (card.Format, dark) switch
        {
            ("Image", true) => (44, 81, 85), ("Image", false) => (180, 224, 220),
            ("Url", true) => (80, 60, 111), ("Url", false) => (216, 196, 242),
            ("Files", true) => (41, 76, 114), ("Files", false) => (184, 215, 249),
            (_, true) => (107, 74, 43), (_, false) => (250, 220, 155)
        };
        border.Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, (byte)r, (byte)g, (byte)b));
    }
    private async void AppIconLoaded(object sender, RoutedEventArgs e)
    {
        if (sender is not Image image || image.DataContext is not ClipboardCard card) return;
        image.Source = null;
        var fallback = (image.Parent as Grid)?.Children.OfType<FontIcon>().FirstOrDefault();
        if (fallback is not null) fallback.Visibility = Visibility.Visible;
        var icon = await ClipboardAppIcons.GetAsync(card.Entry);
        if (!image.IsLoaded || !ReferenceEquals(image.DataContext, card)) return;
        image.Source = icon;
        if (fallback is not null) fallback.Visibility = icon is null ? Visibility.Visible : Visibility.Collapsed;
    }
    private void AppIconDataContextChanged(FrameworkElement sender, DataContextChangedEventArgs args) => AppIconLoaded(sender, new RoutedEventArgs());
    private void ThumbnailLoaded(object sender, RoutedEventArgs e)
    {
        if (sender is not Image image || image.DataContext is not ClipboardCard card || card.Entry.Kind != "Image" || controller is null) return;
        try
        {
            image.Source = new BitmapImage { DecodePixelWidth = 320, UriSource = new Uri(controller.Store.PayloadPath(card.Entry, card.Entry.Attachments[0])) };
        }
        catch (Exception error) { SetStatus(error.Message); }
    }
    private void ThumbnailUnloaded(object sender, RoutedEventArgs e) { if (sender is Image image) image.Source = null; }
    private void ThumbnailDataContextChanged(FrameworkElement sender, DataContextChangedEventArgs args)
    {
        if (sender is Image image) { image.Source = null; ThumbnailLoaded(image, new RoutedEventArgs()); }
    }
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
                OwnedPickerOpen = true; try { var folder = await picker.PickSingleFolderAsync(); if (folder is not null) path.Text = folder.Path; } finally { OwnedPickerOpen = false; }
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
        try { await ShowDialogAsync(dialog); }
        catch (Exception error) { SetStatus(error.Message); }
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
        closed = true; Suspend();
        if (searchTask is not null) await searchTask;
        if (controller is not null) { controller.Store.Changed -= StoreChanged; controller.Status -= SetStatus; }
    }
}
