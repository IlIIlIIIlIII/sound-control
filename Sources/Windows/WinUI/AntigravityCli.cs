using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace SoundControl;

internal static class AntigravityCli
{
    private static Process CreateProcess(BridgeSettings settings, IEnumerable<string> args, string? workspace = null)
    {
        var directory = workspace ?? Path.Combine(BridgeSettings.DirectoryPath, "workspace");
        Directory.CreateDirectory(directory);
        var info = new ProcessStartInfo(settings.CliPath) { UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8, WorkingDirectory = directory };
        foreach (var arg in args) info.ArgumentList.Add(arg);
        // Conservative bound includes Windows argument quoting expansion and executable path.
        if (settings.CliPath.Length + info.ArgumentList.Sum(x => x.Length * 2 + 3) > 30000)
            throw new ArgumentException("Windows CLI 인수 한도를 초과했습니다. 대화 길이를 줄여 주세요.");
        return new Process { StartInfo = info };
    }
    private static void Kill(Process process)
    {
        try { if (!process.HasExited) process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { } catch (System.ComponentModel.Win32Exception) { }
    }
    private static async Task<string> ReadBoundedAsync(StreamReader reader, CancellationToken token)
    {
        var output = new StringBuilder();
        var buffer = new char[4096];
        int count;
        while ((count = await reader.ReadAsync(buffer.AsMemory(), token)) > 0)
        {
            if (output.Length + count > 4 * 1024 * 1024) throw new InvalidOperationException("CLI 출력 크기 제한을 초과했습니다.");
            output.Append(buffer, 0, count);
        }
        return output.ToString();
    }
    public static async Task<string[]> GetModelsAsync(BridgeSettings settings, CancellationToken token)
    {
        settings.Validate();
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(30));
        using var process = CreateProcess(settings, ["models"]);
        process.Start(); process.StandardInput.Close();
        using var registration = timeout.Token.Register(() => Kill(process));
        try
        {
            var stdout = ReadBoundedAsync(process.StandardOutput, timeout.Token);
            var stderr = ReadBoundedAsync(process.StandardError, timeout.Token);
            await Task.WhenAll(stdout, stderr, process.WaitForExitAsync(timeout.Token));
            if (process.ExitCode != 0) throw new InvalidOperationException("CLI 모델 조회 실패. agy를 열어 로그인 상태를 확인해 주세요.");
            var models = stdout.Result.Split('\n').Where(line => line.Contains('\t')).Select(line => line.Split('\t')[0].Trim())
                .Where(id => Regex.IsMatch(id, @"^[a-zA-Z0-9][a-zA-Z0-9._-]+$"));
            return models.Prepend("antigravity").Distinct().ToArray();
        }
        finally { Kill(process); }
    }

    internal sealed record Reply(string Content, object? Usage);
    internal static async Task<Reply> RunAsync(BridgeSettings settings, BridgeRequest request, Func<string, Task> delta, CancellationToken token, string? workspace = null, string? schema = null)
    {
        var args = new List<string> { "--output-format", "stream-json", "--sandbox", "--disable-slash-commands", "--print-timeout", settings.TimeoutSeconds + "s" };
        if (request.Model != "antigravity") args.AddRange(["--model", request.Model]);
        if (workspace is not null) args.AddRange(["--mode", "plan"]);
        if (schema is not null) args.AddRange(["--json-schema", schema]);
        args.AddRange(["-p", request.Prompt]);
        using var process = CreateProcess(settings, args, workspace);
        process.Start(); process.StandardInput.Close();
        using var registration = token.Register(() => Kill(process));
        var stderr = ReadBoundedAsync(process.StandardError, token);
        // Observe failures immediately; a noisy stderr must not deadlock a blocked child.
        _ = stderr.ContinueWith(_ => Kill(process), CancellationToken.None, TaskContinuationOptions.OnlyOnFaulted, TaskScheduler.Default);
        JsonElement? result = null;
        bool emitted = false;
        int size = 0;
        try
        {
            while (await process.StandardOutput.ReadLineAsync(token) is { } line)
            {
                size += line.Length;
                if (size > 4 * 1024 * 1024) throw new InvalidOperationException("CLI 출력 크기 제한을 초과했습니다.");
                JsonDocument doc;
                try { doc = JsonDocument.Parse(line); } catch (JsonException) { continue; }
                using (doc)
                {
                    var root = doc.RootElement;
                    if (root.ValueKind != JsonValueKind.Object) continue;
                    if (root.TryGetProperty("event", out var ev) && ev.GetString() == "step_update" && root.TryGetProperty("step_update", out var step)
                        && step.TryGetProperty("step_type", out var type) && type.GetString() == "agent_response" && step.TryGetProperty("text_delta", out var text) && text.ValueKind == JsonValueKind.String)
                    { emitted = true; await delta(text.GetString()!); }
                    if (root.TryGetProperty("result", out var final)) result = final.Clone();
                }
            }
            await process.WaitForExitAsync(token);
            await stderr;
            if (process.ExitCode != 0) throw new InvalidOperationException($"Antigravity CLI 종료 코드 {process.ExitCode}. CLI 로그인과 모델 설정을 확인해 주세요.");
            if (result is not { } payload || !payload.TryGetProperty("status", out var status) || status.GetString() != "SUCCESS")
                throw new InvalidOperationException("Antigravity CLI가 성공 결과를 반환하지 않았습니다. 로그인, 사용량 및 권한 요청을 확인해 주세요.");
            string content = payload.TryGetProperty("response", out var response) ? response.GetString() ?? "" : "";
            if (!emitted) await delta(content);
            object? usage = null;
            if (payload.TryGetProperty("usage", out var u))
            {
                long Count(string name) => u.TryGetProperty(name, out var value) && value.TryGetInt64(out var n) ? n : 0;
                long input = Count("input_tokens") + Count("cache_read_tokens"), output = Count("output_tokens");
                usage = new { prompt_tokens = input, completion_tokens = output, total_tokens = Count("total_tokens"),
                    prompt_tokens_details = new { cached_tokens = Count("cache_read_tokens") }, completion_tokens_details = new { reasoning_tokens = Count("thinking_tokens") } };
            }
            return new Reply(content, usage);
        }
        finally { Kill(process); try { await stderr; } catch { /* Preserve the primary error. */ } }
    }
}
