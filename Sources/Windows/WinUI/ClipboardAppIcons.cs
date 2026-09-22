using System.Diagnostics;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage;
using Windows.Storage.FileProperties;

namespace SoundControl;

// Shell thumbnails preserve the application's own icon, including packaged apps.
internal static class ClipboardAppIcons
{
    private static readonly Dictionary<string, Task<BitmapImage?>> cache = new(StringComparer.OrdinalIgnoreCase);
    internal static async Task<BitmapImage?> GetAsync(ClipEntry entry)
    {
        string key = entry.SourcePath ?? entry.SourceApp;
        if (!cache.TryGetValue(key, out var task)) cache[key] = task = LoadAsync(entry);
        var result = await task;
        if (result is null) cache.Remove(key);
        return result;
    }
    private static async Task<BitmapImage?> LoadAsync(ClipEntry entry)
    {
        try
        {
            string? path = await Task.Run(() => Resolve(entry));
            if (path is null) return await PackageIconAsync(entry.SourceApp);
            var file = await StorageFile.GetFileFromPathAsync(path);
            using var thumbnail = await file.GetThumbnailAsync(ThumbnailMode.SingleItem, 64, ThumbnailOptions.UseCurrentScale);
            if (thumbnail is null || thumbnail.Size == 0) return await PackageIconAsync(entry.SourceApp);
            var image = new BitmapImage();
            await image.SetSourceAsync(thumbnail);
            return image;
        }
        catch { return await PackageIconAsync(entry.SourceApp); }
    }
    private static async Task<BitmapImage?> PackageIconAsync(string sourceApp)
    {
        try
        {
            var package = await Task.Run(() =>
            {
                foreach (var candidate in new Windows.Management.Deployment.PackageManager().FindPackagesForUser(""))
                {
                    try
                    {
                        var manifest = System.Xml.Linq.XDocument.Load(Path.Combine(candidate.InstalledLocation.Path, "AppxManifest.xml"));
                        if (manifest.Descendants().Any(x => x.Name.LocalName == "Application" &&
                            string.Equals(Path.GetFileName((string?)x.Attribute("Executable")), sourceApp, StringComparison.OrdinalIgnoreCase))) return candidate;
                    }
                    catch { }
                }
                return null;
            });
            if (package is null) return null;
            var app = (await package.GetAppListEntriesAsync()).FirstOrDefault();
            if (app is null) return null;
            using var stream = await app.DisplayInfo.GetLogo(new Windows.Foundation.Size(64, 64)).OpenReadAsync();
            var image = new BitmapImage(); await image.SetSourceAsync(stream); return image;
        }
        catch { return null; }
    }
    private static string? Resolve(ClipEntry entry)
    {
        if (entry.SourcePath is { } saved && Path.IsPathFullyQualified(saved) && File.Exists(saved)) return saved;
        string name = Path.GetFileNameWithoutExtension(entry.SourceApp);
        foreach (var process in Process.GetProcessesByName(name))
        {
            using (process)
            {
                try { if (process.MainModule?.FileName is { } path) return path; }
                catch { }
            }
        }
        // Older archive entries stored only the process name. Windows App Paths
        // can resolve installed desktop apps after their process has exited.
        foreach (var hive in new[] { Microsoft.Win32.Registry.CurrentUser, Microsoft.Win32.Registry.LocalMachine })
        {
            using var key = hive.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\" + Path.GetFileName(entry.SourceApp));
            if (key?.GetValue("") is string path && File.Exists(path.Trim('"'))) return path.Trim('"');
        }
        string system = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), Path.GetFileName(entry.SourceApp));
        return File.Exists(system) ? system : null;
    }
}
