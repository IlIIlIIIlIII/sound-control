using System.Text.Json;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Net.Sockets;
using SoundControl;

// A real child process verifies the CLI contract, including cwd and file access.
if (args.Contains("models")) { Console.WriteLine(ClipboardSearch.Model + "\tGemini 3.8 Flash High"); return; }
if (args.Contains("--output-format"))
{
    string prompt = args[Array.IndexOf(args, "-p") + 1];
    if (prompt.Contains("SLOW_FIXTURE")) { await Task.Delay(60000); return; }
    if (prompt.Contains("DENIED_FIXTURE"))
    {
        Console.WriteLine(JsonSerializer.Serialize(new { result = new { status = "SUCCESS", response = "", denied_actions = new[] { new { action = "read" } } } }));
        return;
    }
    if (!args.Contains("--json-schema") || !args.Contains("plan") || !args.Contains("--sandbox") || !args.Contains(ClipboardSearch.Model)) { Environment.ExitCode = 9; return; }
    if (!prompt.Contains("untrusted data") || prompt.Contains("CORAL-729")) { Environment.ExitCode = 10; return; }
    var matching = Directory.EnumerateFiles(Environment.CurrentDirectory, "content.txt", SearchOption.AllDirectories)
        .Where(x => File.ReadAllText(x).Contains("CORAL-729")).ToArray();
    string response = JsonSerializer.Serialize(new { answer = "파일에서 확인한 코드: CORAL-729", itemIds = matching.Select(x => Path.GetFileName(Path.GetDirectoryName(x))).ToArray(), limitations = Array.Empty<string>() });
    Console.WriteLine(JsonSerializer.Serialize(new { result = new { status = "SUCCESS", response = "", structured_output = JsonSerializer.Deserialize<JsonElement>(response) } }));
    return;
}

int checks = 0;
void Check(bool condition, string name) { if (!condition) throw new Exception("FAIL: " + name); Console.WriteLine("PASS: " + name); checks++; }
async Task Fails(Func<Task> action, string name)
{
    bool failed = false; try { await action(); } catch (Exception e) when (e is InvalidDataException or IOException or ArgumentException or InvalidOperationException or OperationCanceledException or TimeoutException) { failed = true; }
    Check(failed, name);
}
// Keep generated test artifacts for inspection; never touch the user's real archive.
string root = Path.Combine(Path.GetTempPath(), "PersonalToolsClipboardTests", Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
var settings = new ClipboardSettings { ArchivePath = Path.Combine(root, "archive") };
var store = new ClipboardStore(settings);
await store.LoadAsync();
string note = "회의 메모\n산호 프로젝트 예약 코드: CORAL-729\n" + new string('가', 8200) + "끝부분검색";
var text = (await store.SaveAsync(new("Text", "notepad.exe", Text: note)))!;
Check(await File.ReadAllTextAsync(store.PayloadPath(text, text.Attachments[0])) == note, "UTF-8 multiline roundtrip");
Check(await store.SaveAsync(new("Text", "notepad.exe", Text: note)) is null, "consecutive duplicate suppression");
Check((await store.FindAsync("끝부분검색", default)).Single().Id == text.Id, "full text beyond preview / buffer boundary");
byte[] png = Convert.FromBase64String("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII=");
var image = (await store.SaveAsync(new("Image", "paint.exe", Image: png)))!;
Check((await File.ReadAllBytesAsync(store.PayloadPath(image, image.Attachments[0]))).SequenceEqual(png), "image byte integrity");
string original = Path.Combine(root, "한글 파일.txt"); await File.WriteAllTextAsync(original, "파일 사본 테스트");
var files = (await store.SaveAsync(new("Files", "explorer.exe", Files: [original])))!;
await File.WriteAllTextAsync(original, "원본 변경");
Check(await File.ReadAllTextAsync(store.PayloadPath(files, files.Attachments[0])) == "파일 사본 테스트", "independent file copy");
await store.SetPinnedAsync(text, true);
store = new ClipboardStore(settings); await store.LoadAsync();
Check(store.Entries.Count == 3 && store.Entries.Single(x => x.Id == text.Id).Pinned, "restart recovers metadata and pin");
var pending = Path.Combine(settings.ArchivePath, ".pending-incomplete"); Directory.CreateDirectory(pending);
await File.WriteAllTextAsync(Path.Combine(pending, "metadata.json"), "{");
await store.LoadAsync(); Check(store.Entries.Count == 3, "incomplete commits invisible");
await Fails(() => store.SaveAsync(new("Files", "test", Files: [root])), "directory capture rejected");
await Fails(() => store.SaveAsync(new("Files", "test", Files: [Path.Combine(root, "missing.txt")])), "unreadable source is not committed");
var tiny = new ClipboardStore(settings with { ArchivePath = Path.Combine(root, "tiny"), ItemLimitBytes = 4 });
await Fails(() => tiny.SaveAsync(new("Text", "test", Text: "too large")), "per item limit");
Check(!Directory.EnumerateDirectories(tiny.Root).Any(), "failed capture cleans staging");
var full = new ClipboardStore(settings with { ArchivePath = Path.Combine(root, "full"), TotalLimitBytes = 4 });
await Fails(() => full.SaveAsync(new("Text", "test", Text: "x")), "archive limit includes metadata");
await store.UpdateSettingsAsync(settings with { Paused = true }, _ => { });
Check(await store.SaveAsync(new("Text", "test", Text: "paused")) is null, "pause recording");
await store.UpdateSettingsAsync(settings, _ => { });
var moved = settings with { ArchivePath = Path.Combine(root, "moved") };
await store.UpdateSettingsAsync(moved, _ => { });
Check(store.Root == moved.ArchivePath && File.Exists(Path.Combine(settings.ArchivePath, text.RelativeDirectory, "content.txt")), "migration keeps originals");
Check(await File.ReadAllTextAsync(store.PayloadPath(text, text.Attachments[0])) == note, "migration payload verified");
await Fails(() => store.UpdateSettingsAsync(settings, _ => { }), "nonempty migration destination rejected");
Check(store.Root == moved.ArchivePath, "failed migration preserves active root");
await Fails(() => store.UpdateSettingsAsync(moved with { ArchivePath = Path.Combine(root, "persist-fails") }, _ => throw new IOException("fixture persist failure")), "settings write failure");
Check(store.Root == moved.ArchivePath, "settings failure does not switch root");
await Fails(() => Task.Run(() => ClipboardStore.SafePath(store.Root, "../outside")), "path traversal rejected");
await store.DeleteAsync(image); Check(!store.Entries.Any(x => x.Id == image.Id), "delete removes record");
var parsed = ClipboardSearch.Parse(JsonSerializer.Serialize(new { answer = "answer", itemIds = new[] { text.Id, "unknown", text.Id }, limitations = Array.Empty<string>() }), store.Entries);
Check(parsed.ItemIds.SequenceEqual([text.Id]) && parsed.Limitations.Length == 1, "unknown result IDs removed and deduplicated");
await Fails(() => Task.Run(() => ClipboardSearch.Parse("not-json", store.Entries)), "invalid search output rejected");

var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start(); int port = ((IPEndPoint)listener.LocalEndpoint).Port; listener.Stop();
bool live = args.Contains("--live");
var bridgeSettings = new BridgeSettings(live ? BridgeSettings.DefaultCli : Environment.ProcessPath!, port, TimeoutSeconds: live ? 120 : 10);
await using var bridge = new AntigravityBridge();
var search = new ClipboardSearch(bridge, store, () => bridgeSettings);
var result = await search.SearchClipboardAsync("산호 프로젝트 예약 코드를 찾아줘. 실제 content.txt 내용을 읽고 항목 ID와 코드를 답해줘.", default);
Check(result.ItemIds.Contains(text.Id) && result.Answer.Contains("CORAL-729"), live ? "LIVE: agent reads archive files and returns correct ID + content" : "CLI cwd, exact model, read-only arguments, file reading and result mapping");
if (!live)
{
    try { await search.SearchClipboardAsync("DENIED_FIXTURE", default); Check(false, "denied tool produces actionable message"); }
    catch (InvalidOperationException error) { Check(error.Message.Contains("권한"), "denied tool produces actionable message instead of invalid JSON"); }
    var emptyScope = await search.SearchClipboardAsync("empty scope", Array.Empty<string>(), default);
    Check(emptyScope.ItemIds.Length == 0, "empty AI scope returns without invoking bridge");
    var outsideScope = await search.SearchClipboardAsync("산호 프로젝트", new[] { files.Id }, default);
    Check(outsideScope.ItemIds.Length == 0 && !outsideScope.Answer.Contains("CORAL-729"), "AI excludes out-of-scope IDs and untrusted summary");
    using var cancel = new CancellationTokenSource(500);
    await Fails(() => search.SearchClipboardAsync("SLOW_FIXTURE", cancel.Token), "search cancellation kills child");
    await bridge.StartAsync(bridgeSettings);
    using var client = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}") };
    client.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", bridge.ApiKey);
    using var busyCancellation = new CancellationTokenSource();
    var busy = search.SearchClipboardAsync("SLOW_FIXTURE", busyCancellation.Token);
    await Task.Delay(300);
    using var response = await client.PostAsJsonAsync("/v1/chat/completions", new { messages = new[] { new { role = "user", content = "test" } } });
    Check(response.StatusCode == HttpStatusCode.TooManyRequests, "search shares HTTP execution slot");
    busyCancellation.Cancel(); await Fails(async () => await busy, "busy search canceled");
    await Fails(() => search.SearchClipboardAsync("SLOW_FIXTURE", default), "search timeout");
    result = await search.SearchClipboardAsync("산호 프로젝트 예약 코드를 찾아줘", default);
    Check(result.ItemIds.Contains(text.Id), "slot reusable after timeout");
    Check(!Directory.EnumerateFiles(store.Root, ".search-*.json").Any(), "search manifests cleaned after success, denial, cancellation and timeout");
}
else
{
    // Deliberately unrelated and duplicate filenames; answers exist only in copied payloads.
    string fixtures = Path.Combine(root, "file-search-fixtures");
    Directory.CreateDirectory(fixtures);
    string evidence = Path.Combine(fixtures, "notes.txt");
    await File.WriteAllTextAsync(evidence, new string('가', 9000) + "\n오로라 프로젝트 배포 승인 코드: AURORA-582. 배포 담당자는 김서윤입니다.");
    var relevant = (await store.SaveAsync(new("Files", "explorer.exe", Files: [evidence])))!;
    await File.WriteAllTextAsync(evidence, "오로라 프로젝트 관련 회의는 취소되었습니다. 배포 승인 정보는 없습니다.");
    var decoy = (await store.SaveAsync(new("Files", "explorer.exe", Files: [evidence])))!;
    string env = Path.Combine(fixtures, ".env.example");
    await File.WriteAllTextAsync(env, "# Synthetic test data, not credentials\nORBIT_REGION=seoul-test-17\n");
    var hidden = (await store.SaveAsync(new("Files", "explorer.exe", Files: [env])))!;
    var manifest = ClipboardSearch.BuildPrompt("파일 내용 검색", [relevant, hidden]);
    Check(manifest.Contains(".env.example") && manifest.Contains(relevant.Id) && !manifest.Contains("AURORA-582"), "file manifest includes names and IDs without injecting answer payload");
    result = await search.SearchClipboardAsync("복사한 파일 본문에서 오로라 프로젝트 배포 승인 코드와 담당자를 찾아줘. 이름이 같은 파일도 내용을 구별해줘.", [relevant.Id, decoy.Id, hidden.Id], default);
    Check(result.ItemIds.SequenceEqual([relevant.Id]) && result.Answer.Contains("AURORA-582") && result.Answer.Contains("김서윤"), "LIVE: file body beyond preview, duplicate filenames and nonmatching decoy");
    Console.WriteLine("LIVE file body answer: " + result.Answer);
    result = await search.SearchClipboardAsync(".env.example 파일을 찾아서 ORBIT_REGION 값을 알려줘.", [relevant.Id, hidden.Id], default);
    Check(result.ItemIds.SequenceEqual([hidden.Id]) && result.Answer.Contains("seoul-test-17"), "LIVE: dot-file original filename and copied attachment content");
    Console.WriteLine("LIVE dot-file answer: " + result.Answer);
    result = await search.SearchClipboardAsync("오로라 프로젝트 배포 승인 코드가 들어 있는 파일을 찾아줘.", [decoy.Id], default);
    Check(result.ItemIds.Length == 0 && !result.Answer.Contains("AURORA-582"), "LIVE: category scope excludes actual match and returns no fabricated match");
}
await ClipboardFeatureChecks.RunAsync(root, Check, Fails);
Console.WriteLine($"{checks} clipboard checks passed. Test archive: {store.Root}");
