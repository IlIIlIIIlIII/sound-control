using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace SoundControl;

public sealed record ClipboardSettings
{
    public static string SettingsDirectory => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PersonalTools", "Clipboard");
    public string ArchivePath { get; init; } = Path.Combine(SettingsDirectory, "Archive");
    public bool Paused { get; init; }
    public string[] ExcludedApps { get; init; } = [];
    public uint HotkeyModifiers { get; init; } = 6; // Control + Shift
    public uint HotkeyKey { get; init; } = 0x56;
    public long ItemLimitBytes { get; init; } = 100L * 1024 * 1024;
    public long TotalLimitBytes { get; init; } = 10L * 1024 * 1024 * 1024;
    public static ClipboardSettings Load()
    {
        var path = Path.Combine(SettingsDirectory, "settings.json");
        return File.Exists(path) ? JsonSerializer.Deserialize<ClipboardSettings>(File.ReadAllText(path)) ?? new() : new();
    }
    public void Save()
    {
        Directory.CreateDirectory(SettingsDirectory);
        var path = Path.Combine(SettingsDirectory, "settings.json");
        File.WriteAllText(path + ".tmp", JsonSerializer.Serialize(this));
        File.Move(path + ".tmp", path, true);
    }
}

public sealed record ClipAttachment(string OriginalName, string RelativePath, long Length, string Sha256);
public sealed record ClipEntry(string Id, string Kind, DateTimeOffset CreatedAt, string SourceApp,
    string Preview, string ContentHash, string RelativeDirectory, ClipAttachment[] Attachments, bool Pinned = false)
{
    public string Label => $"{(Pinned ? "★ " : "")}{Preview}";
    public string Detail => $"{Kind} · {CreatedAt.ToLocalTime():MM-dd HH:mm} · {SourceApp}";
}
public sealed record ClipCapture(string Kind, string SourceApp, string? Text = null, byte[]? Image = null, string[]? Files = null);

// The committed directories are the source of truth; no database is required to recover a history.
public sealed class ClipboardStore
{
    private readonly SemaphoreSlim gate = new(1);
    private ClipEntry[] entries = [];
    private string? lastHash;
    public ClipboardSettings Settings { get; private set; }
    public string Root => Path.GetFullPath(Settings.ArchivePath);
    public IReadOnlyList<ClipEntry> Entries => entries;
    public event Action? Changed;
    public event Action<string>? Warning;
    public ClipboardStore(ClipboardSettings settings) => Settings = settings;
    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    private static readonly EnumerationOptions Enumeration = new() { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint, IgnoreInaccessible = false };

    internal static string SafePath(string root, string relative)
    {
        root = Path.GetFullPath(root);
        var path = Path.GetFullPath(Path.Combine(root, relative));
        if (!path.StartsWith(Path.TrimEndingDirectorySeparator(root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("저장소 밖의 경로는 사용할 수 없습니다.");
        for (var cursor = path; cursor is not null; cursor = Path.GetDirectoryName(cursor))
        {
            if ((File.Exists(cursor) || Directory.Exists(cursor)) && (File.GetAttributes(cursor) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("저장소의 링크 경로는 사용할 수 없습니다.");
            if (cursor.Equals(root, StringComparison.OrdinalIgnoreCase)) break;
        }
        return path;
    }
    public string PayloadPath(ClipEntry item, ClipAttachment attachment) => SafePath(Root, Path.Combine(item.RelativeDirectory, attachment.RelativePath));
    public async Task<ClipEntry[]> FindAsync(string query, CancellationToken token)
    {
        if (string.IsNullOrEmpty(query)) return entries;
        var matches = new List<ClipEntry>();
        foreach (var item in entries)
        {
            token.ThrowIfCancellationRequested();
            if (item.Preview.Contains(query, StringComparison.OrdinalIgnoreCase) || item.SourceApp.Contains(query, StringComparison.OrdinalIgnoreCase)
                || item.Attachments.Any(x => x.OriginalName.Contains(query, StringComparison.OrdinalIgnoreCase)))
            { matches.Add(item); continue; }
            if (item.Kind != "Text") continue;
            try
            {
                using var reader = File.OpenText(PayloadPath(item, item.Attachments[0]));
                var buffer = new char[8192]; string tail = "";
                int count;
                while ((count = await reader.ReadAsync(buffer.AsMemory(), token)) > 0)
                {
                    string part = tail + new string(buffer, 0, count);
                    if (part.Contains(query, StringComparison.OrdinalIgnoreCase)) { matches.Add(item); break; }
                    tail = part[^Math.Min(part.Length, query.Length - 1)..];
                }
            }
            catch (IOException) { /* A record may have been deleted while filtering. */ }
        }
        return matches.ToArray();
    }
    public async Task LoadAsync(CancellationToken token = default)
    {
        await gate.WaitAsync(token);
        try
        {
            Directory.CreateDirectory(Root);
            var loaded = new List<ClipEntry>();
            foreach (var day in Directory.EnumerateDirectories(Root))
            {
                if (!Regex.IsMatch(Path.GetFileName(day), "^[0-9]{8}$")) continue;
                SafePath(Root, Path.GetRelativePath(Root, day));
                foreach (var directory in Directory.EnumerateDirectories(day))
                {
                    token.ThrowIfCancellationRequested();
                    if (!Guid.TryParseExact(Path.GetFileName(directory), "N", out _)) continue;
                    try
                    {
                        var relative = Path.GetRelativePath(Root, directory);
                        var metadata = SafePath(Root, Path.Combine(relative, "metadata.json"));
                        var item = JsonSerializer.Deserialize<ClipEntry>(await File.ReadAllTextAsync(metadata, token)) ?? throw new InvalidDataException();
                        if (item.Id != Path.GetFileName(directory) || item.RelativeDirectory != relative || item.Kind is not ("Text" or "Image" or "Files") || item.Attachments.Length == 0)
                            throw new InvalidDataException();
                        foreach (var file in item.Attachments)
                            if (!File.Exists(PayloadPath(item, file))) throw new InvalidDataException();
                        loaded.Add(item);
                    }
                    catch (Exception e) when (e is InvalidDataException or IOException or JsonException or ArgumentException or UnauthorizedAccessException or NullReferenceException)
                    { Warning?.Invoke("읽을 수 없는 클립보드 기록을 건너뛰었습니다: " + Path.GetFileName(directory)); }
                }
            }
            entries = loaded.OrderByDescending(x => x.CreatedAt).ToArray();
            lastHash = entries.FirstOrDefault()?.ContentHash;
        }
        finally { gate.Release(); }
        Changed?.Invoke();
    }
    public async Task<ClipEntry?> SaveAsync(ClipCapture capture, CancellationToken token = default)
    {
        await gate.WaitAsync(token);
        string? staging = null;
        try
        {
            if (Settings.Paused) return null;
            Directory.CreateDirectory(Root);
            var id = Guid.NewGuid().ToString("N");
            var created = DateTimeOffset.UtcNow;
            var relative = Path.Combine(created.ToString("yyyyMMdd"), id);
            staging = SafePath(Root, ".pending-" + id);
            Directory.CreateDirectory(staging);
            var attachments = new List<ClipAttachment>();
            long payloadBytes = 0;
            async Task Add(string name, string originalName, Stream input)
            {
                var destination = Path.Combine(staging, name);
                await using var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None, 65536, true);
                using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                var buffer = new byte[65536];
                long length = 0;
                int read;
                while ((read = await input.ReadAsync(buffer, token)) > 0)
                {
                    payloadBytes += read; length += read;
                    if (payloadBytes > Settings.ItemLimitBytes) throw new IOException("항목 저장 한도를 초과했습니다.");
                    hash.AppendData(buffer, 0, read);
                    await output.WriteAsync(buffer.AsMemory(0, read), token);
                }
                attachments.Add(new(originalName, name, length, Convert.ToHexString(hash.GetHashAndReset())));
            }
            string preview;
            switch (capture.Kind)
            {
                case "Text":
                    if (string.IsNullOrEmpty(capture.Text)) return null;
                    using (var input = new MemoryStream(Encoding.UTF8.GetBytes(capture.Text))) await Add("content.txt", "content.txt", input);
                    preview = capture.Text.Replace('\r', ' ').Replace('\n', ' ');
                    break;
                case "Image":
                    if (capture.Image is not { Length: > 0 }) return null;
                    using (var input = new MemoryStream(capture.Image)) await Add("image.png", "image.png", input);
                    preview = "이미지";
                    break;
                case "Files":
                    if (capture.Files is not { Length: > 0 }) return null;
                    for (int i = 0; i < capture.Files.Length; i++)
                    {
                        string file = capture.Files[i];
                        if (!File.Exists(file) || (File.GetAttributes(file) & FileAttributes.ReparsePoint) != 0)
                            throw new IOException("일반 파일만 보관할 수 있습니다. 폴더와 링크는 지원하지 않습니다.");
                        if (new FileInfo(file).Length + payloadBytes > Settings.ItemLimitBytes) throw new IOException("항목 저장 한도를 초과했습니다.");
                        await using var input = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 65536, true);
                        await Add($"{i:D3}-" + Path.GetFileName(file), Path.GetFileName(file), input);
                    }
                    preview = string.Join(", ", attachments.Select(x => x.OriginalName));
                    break;
                default: throw new ArgumentException("지원하지 않는 클립보드 형식입니다.");
            }
            var contentHash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(capture.Kind + "\n" + string.Join("\n", attachments.Select(x => x.OriginalName + ":" + x.Sha256)))));
            if (contentHash == lastHash) return null;
            var item = new ClipEntry(id, capture.Kind, created, capture.SourceApp, preview[..Math.Min(preview.Length, 240)], contentHash, relative, attachments.ToArray());
            await File.WriteAllTextAsync(Path.Combine(staging, "metadata.json"), JsonSerializer.Serialize(item, Json), token);
            string description = $"# Clipboard {id}\n\nID: {id}\nKind: {item.Kind}\nCopied: {created:O}\nSource: {capture.SourceApp}\n\n";
            description += capture.Kind == "Text" ? capture.Text : string.Join("\n", attachments.Select(x => $"- {x.OriginalName}: {x.RelativePath} ({x.Length} bytes)"));
            await File.WriteAllTextAsync(Path.Combine(staging, "entry.md"), description, new UTF8Encoding(false), token);
            long total = Directory.EnumerateFiles(Root, "*", Enumeration).Sum(x => new FileInfo(x).Length);
            if (total > Settings.TotalLimitBytes) throw new IOException("전체 저장 한도를 초과했습니다. 기록을 삭제하거나 저장 한도를 늘려 주세요.");
            var destinationDirectory = SafePath(Root, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(destinationDirectory)!);
            token.ThrowIfCancellationRequested();
            Directory.Move(staging, destinationDirectory); staging = null;
            entries = [item, .. entries]; lastHash = contentHash;
            Changed?.Invoke();
            return item;
        }
        finally
        {
            try { if (staging is not null && Directory.Exists(staging)) Directory.Delete(staging, true); }
            finally { gate.Release(); }
        }
    }
    public async Task SetPinnedAsync(ClipEntry entry, bool pinned)
    {
        await gate.WaitAsync();
        try
        {
            var current = entries.FirstOrDefault(x => x.Id == entry.Id) ?? throw new IOException("기록이 삭제되었습니다.");
            var updated = current with { Pinned = pinned };
            string path = SafePath(Root, Path.Combine(current.RelativeDirectory, "metadata.json"));
            await File.WriteAllTextAsync(path + ".tmp", JsonSerializer.Serialize(updated, Json));
            File.Move(path + ".tmp", path, true);
            entries = entries.Select(x => x.Id == entry.Id ? updated : x).ToArray();
        }
        finally { gate.Release(); }
        Changed?.Invoke();
    }
    public async Task DeleteAsync(ClipEntry entry)
    {
        await gate.WaitAsync();
        try
        {
            var current = entries.FirstOrDefault(x => x.Id == entry.Id);
            if (current is null) return;
            var path = SafePath(Root, current.RelativeDirectory);
            // Check every descendant before recursive deletion; never follow a user-added junction.
            foreach (var child in Directory.EnumerateFileSystemEntries(path, "*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = 0 }))
                SafePath(Root, Path.GetRelativePath(Root, child));
            Directory.Delete(path, true);
            entries = entries.Where(x => x.Id != entry.Id).ToArray(); lastHash = null;
        }
        finally { gate.Release(); }
        Changed?.Invoke();
    }
    public async Task UpdateSettingsAsync(ClipboardSettings settings, Action<ClipboardSettings>? persist = null, CancellationToken token = default)
    {
        await gate.WaitAsync(token);
        try
        {
            if (settings.ItemLimitBytes <= 0 || settings.TotalLimitBytes < settings.ItemLimitBytes) throw new ArgumentException("저장 용량 설정을 확인해 주세요.");
            string destination = Path.GetFullPath(settings.ArchivePath);
            if (!destination.Equals(Root, StringComparison.OrdinalIgnoreCase))
            {
                var oldPrefix = Path.TrimEndingDirectorySeparator(Root) + Path.DirectorySeparatorChar;
                var newPrefix = Path.TrimEndingDirectorySeparator(destination) + Path.DirectorySeparatorChar;
                if (destination.StartsWith(oldPrefix, StringComparison.OrdinalIgnoreCase) || Root.StartsWith(newPrefix, StringComparison.OrdinalIgnoreCase))
                    throw new ArgumentException("기존 저장소의 상위·하위 폴더로는 이동할 수 없습니다.");
                Directory.CreateDirectory(destination);
                if (Directory.EnumerateFileSystemEntries(destination).Any()) throw new IOException("새 저장 폴더는 비어 있어야 합니다.");
                foreach (var entry in entries)
                {
                    string source = SafePath(Root, entry.RelativeDirectory);
                    foreach (var file in Directory.EnumerateFiles(source, "*", Enumeration))
                    {
                        token.ThrowIfCancellationRequested();
                        string target = SafePath(destination, Path.GetRelativePath(Root, file));
                        SafePath(Root, Path.GetRelativePath(Root, file));
                        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                        File.Copy(file, target, false);
                        await using var a = File.OpenRead(file);
                        await using var b = File.OpenRead(target);
                        if (!(await SHA256.HashDataAsync(a, token)).SequenceEqual(await SHA256.HashDataAsync(b, token)))
                            throw new IOException("저장소 복사 검증에 실패했습니다. 기존 저장소를 유지합니다.");
                    }
                }
            }
            token.ThrowIfCancellationRequested();
            (persist ?? (value => value.Save()))(settings);
            Settings = settings;
        }
        finally { gate.Release(); }
        Changed?.Invoke();
    }
}
