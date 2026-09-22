using System.Text.Json;
using PersonalTools;

internal static class ClipboardFeatureChecks
{
    internal static async Task RunAsync(string root, Action<bool, string> check, Func<Func<Task>, string, Task> fails)
    {
        var settings = new ClipboardSettings { ArchivePath = Path.Combine(root, "features") };
        var store = new ClipboardStore(settings);
        await store.LoadAsync();
        var url = (await store.SaveAsync(new("Text", "browser.exe", Text: "https://example.com/" + new string('a', 9000))))!;
        var sentence = (await store.SaveAsync(new("Text", "notes.exe", Text: "See https://example.com for details", SourcePath: @"C:\Apps\notes.exe")))!;
        var image = (await store.SaveAsync(new("Image", "paint.exe", Image: [1, 2, 3])))!;
        check(url.Kind == "Text" && url.ContentFormat == "Url", "URL preserves text payload format");
        check(await store.GetFormatAsync(url) == "Url", "long URL classified beyond preview");
        check(await store.GetFormatAsync(sentence) == "Text", "embedded URL stays text");
        check(ClipboardStore.ClassifyText("  HTTPS://example.com/a?q=1\r\n") == "Url", "single trimmed HTTP(S) URL");
        check(ClipboardStore.ClassifyText("https://a.example https://b.example") == "Text", "multiple URLs stay text");
        check(ClipboardStore.ClassifyText("file:///C:/Windows") == "Text", "non HTTP schemes stay text");
        var work = await store.SaveCategoryAsync("업무");
        var links = await store.SaveCategoryAsync("링크");
        await store.SetCategoriesAsync(url.Id, [work.Id, links.Id, work.Id]);
        await store.SetCategoriesAsync(sentence.Id, [work.Id]);
        check(store.CategoryIds(url.Id).Length == 2, "multiple categories and duplicate membership suppression");
        var defensiveCopy = store.CategoryIds(url.Id); defensiveCopy[0] = "mutated";
        check(store.CategoryIds(url.Id).Contains(work.Id), "membership callers cannot mutate catalog");
        await fails(() => store.SaveCategoryAsync("업무"), "duplicate category names rejected");
        await fails(() => store.SetCategoriesAsync(url.Id, ["missing"]), "unknown category rejected");
        await fails(() => store.SaveCategoryAsync("  "), "blank category name rejected");
        var renamed = await store.SaveCategoryAsync("프로젝트", work.Id);
        check(renamed.Id == work.Id, "rename preserves category identity");
        var today = DateOnly.FromDateTime(DateTime.Now);
        var filtered = await store.FindAsync("example", new ClipboardFilter { CategoryId = work.Id, Processes = ["BROWSER.EXE", "paint.exe"], Formats = ["Url", "Image"], From = today, Through = today }, default);
        check(filtered.Select(x => x.Id).SequenceEqual([url.Id]), "filters AND together and OR within process/format lists");
        check((await store.FindAsync("", new ClipboardFilter { Formats = ["Text"] }, default)).Single().Id == sentence.Id, "URL excluded from plain-text filter");
        check((await store.FindAsync("", new ClipboardFilter { Formats = ["Url", "Image"] }, default)).Length == 2, "format multi-select");
        check((await store.FindAsync("notes.exe", default)).Single().Id == sentence.Id, "query matches copied process");
        await store.SetPinnedAsync(url, true);
        check((await store.FindAsync("", new ClipboardFilter { PinnedOnly = true, CategoryId = links.Id }, default)).Single().Id == url.Id, "pinned collection intersection");
        await fails(() => store.FindAsync("", new ClipboardFilter { From = today, Through = today.AddDays(-1) }, default), "reversed date range rejected");
        var day = new DateTime(2026, 9, 22);
        var offset = TimeZoneInfo.Local.GetUtcOffset(day);
        var boundaryFilter = new ClipboardFilter { From = new(2026, 9, 22), Through = new(2026, 9, 22) };
        check(boundaryFilter.MatchesMetadata(url with { CreatedAt = new DateTimeOffset(day, offset).ToUniversalTime() }), "local date includes start midnight");
        check(boundaryFilter.MatchesMetadata(url with { CreatedAt = new DateTimeOffset(day.AddDays(1).AddTicks(-1), offset).ToUniversalTime() }), "local date includes end of day");
        check(!boundaryFilter.MatchesMetadata(url with { CreatedAt = new DateTimeOffset(day.AddDays(1), offset).ToUniversalTime() }), "local date excludes next midnight");
        using (var cancelled = new CancellationTokenSource())
        {
            cancelled.Cancel();
            await fails(() => store.FindAsync("", new ClipboardFilter(), cancelled.Token), "local search cancellation");
        }
        // Simulate archives written before ContentFormat existed, including a long misleading prefix.
        string metadata = Path.Combine(settings.ArchivePath, url.RelativeDirectory, "metadata.json");
        var legacy = JsonSerializer.SerializeToNode(store.Entries.Single(x => x.Id == url.Id))!;
        legacy.AsObject().Remove("ContentFormat");
        await File.WriteAllTextAsync(metadata, legacy.ToJsonString());
        store = new(settings); await store.LoadAsync();
        var oldUrl = store.Entries.Single(x => x.Id == url.Id);
        check(store.Entries.Single(x => x.Id == sentence.Id).SourcePath == @"C:\Apps\notes.exe" && oldUrl.SourcePath is null, "source executable metadata survives restart and legacy entries remain compatible");
        check(oldUrl.ContentFormat is null && await store.GetFormatAsync(oldUrl) == "Url", "old archive lazy full-payload URL classification");
        check(store.CategoryIds(url.Id).Length == 2 && store.Categories.Single(x => x.Id == work.Id).Name == "프로젝트", "categories survive restart");
        var misleading = (await store.SaveAsync(new("Text", "browser.exe", Text: "https://example.com/" + new string('b', 8000) + " followed by prose")))!;
        check(await store.GetFormatAsync(misleading with { ContentFormat = null }) == "Text", "legacy classification does not trust truncated URL preview");
        var moved = settings with { ArchivePath = Path.Combine(root, "feature-moved") };
        await store.UpdateSettingsAsync(moved, _ => { });
        store = new(moved); await store.LoadAsync();
        check(store.CategoryIds(url.Id).Length == 2 && store.Categories.Count == 2, "archive relocation copies categories and membership");
        await store.DeleteCategoryAsync(work.Id);
        check(store.Entries.Count == 4 && store.CategoryIds(url.Id).SequenceEqual([links.Id]) && store.CategoryIds(sentence.Id).Length == 0, "category deletion preserves records and other memberships");
        await store.DeleteAsync(store.Entries.Single(x => x.Id == url.Id));
        store = new(moved); await store.LoadAsync();
        check(store.CategoryIds(url.Id).Length == 0 && store.Entries.Count == 3, "entry deletion removes membership durably");
        check(store.Categories.Single().Id == links.Id, "empty collection survives restart");
        var negative = ClipboardPanelBounds.AtBottom(-2560, -500, 2560, 1400, 144);
        check(negative == new ClipboardPanelBounds(-2560, 390, 2560, 510), "negative-origin monitor and 150 percent DPI");
        check(ClipboardPanelBounds.AtBottom(48, 0, 1872, 1040, 96) == new ClipboardPanelBounds(48, 700, 1872, 340), "work area respects side and bottom taskbars");
        check(ClipboardPanelBounds.AtBottom(0, 0, 800, 300, 192).Height == 300, "short monitor clamps panel height");
        check(ClipboardPanelBounds.AtBottom(0, 0, 1920, 1080, 0).Height == 340, "invalid DPI uses 96 default");
        check(ClipboardPanelBounds.AtBottom(-2560, -500, 2560, 1400, 144, floating: true) == new ClipboardPanelBounds(-2542, 372, 2524, 510), "floating acrylic margin scales on negative-origin display");
        check(ClipboardPanelBounds.AtBottom(0, 0, 800, 300, 192, floating: true) == new ClipboardPanelBounds(24, 24, 752, 252), "floating panel stays inside short work area");
    }
}

