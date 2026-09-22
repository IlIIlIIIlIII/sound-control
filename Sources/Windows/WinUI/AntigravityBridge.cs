using System.Diagnostics;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;

namespace PersonalTools;

public sealed record BridgeSettings(string CliPath, int Port = 8877, string Model = "antigravity", int TimeoutSeconds = 120)
{
    public static string DirectoryPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PersonalTools", "Antigravity");
    public static string DefaultCli => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "agy", "bin", "agy.exe");
    public static string LoadOrCreateApiKey()
    {
        Directory.CreateDirectory(DirectoryPath);
        var path = Path.Combine(DirectoryPath, "api-key.txt");
        if (!File.Exists(path))
        {
            var key = "sk-local-" + Convert.ToHexString(RandomNumberGenerator.GetBytes(24)).ToLowerInvariant();
            using var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            file.Write(Encoding.UTF8.GetBytes(key));
        }
        var saved = File.ReadAllText(path).Trim();
        if (!Regex.IsMatch(saved, "^sk-local-[a-f0-9]{48}$")) throw new InvalidDataException("저장된 브릿지 API 키 형식이 올바르지 않습니다.");
        return saved;
    }
    public static BridgeSettings Load()
    {
        var path = Path.Combine(DirectoryPath, "settings.json");
        return File.Exists(path) ? JsonSerializer.Deserialize<BridgeSettings>(File.ReadAllText(path)) ?? new(DefaultCli) : new(DefaultCli);
    }
    public void Save()
    {
        Directory.CreateDirectory(DirectoryPath);
        var path = Path.Combine(DirectoryPath, "settings.json");
        File.WriteAllText(path + ".tmp", JsonSerializer.Serialize(this));
        File.Move(path + ".tmp", path, true);
    }
    public void Validate()
    {
        if (!File.Exists(CliPath) || !Path.GetExtension(CliPath).Equals(".exe", StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("Antigravity CLI의 agy.exe 경로를 확인해 주세요.");
        if (Port is < 1024 or > 65535) throw new ArgumentException("포트는 1024~65535 사이여야 합니다.");
        if (TimeoutSeconds is < 10 or > 600) throw new ArgumentException("제한 시간은 10~600초 사이여야 합니다.");
        if (string.IsNullOrWhiteSpace(Model) || Model.Length > 200) throw new ArgumentException("기본 모델을 입력해 주세요.");
    }
}

// Protocol reference: starhunt/star-cliproxy agy-provider.ts (see docs/antigravity-bridge.md).
// Native implementation: no shell, OAuth extraction, external proxy, or permission bypass.
public sealed class AntigravityBridge : IAsyncDisposable
{
    private WebApplication? server;
    private CancellationTokenSource? lifetime;
    private readonly SemaphoreSlim slot = new(1);
    private readonly SemaphoreSlim lifecycle = new(1);
    public string ApiKey { get; }
    public AntigravityBridge(string? apiKey = null) => ApiKey = apiKey ?? "sk-local-" + Convert.ToHexString(RandomNumberGenerator.GetBytes(24)).ToLowerInvariant();
    public bool IsRunning => server is not null;
    public event Action<string>? Status;

    public async Task StartAsync(BridgeSettings settings, CancellationToken token = default)
    {
        await lifecycle.WaitAsync(token);
        try
        {
        if (IsRunning) throw new InvalidOperationException("브릿지가 이미 실행 중입니다.");
        settings.Validate();
        var builder = WebApplication.CreateSlimBuilder(new WebApplicationOptions { Args = [], ContentRootPath = AppContext.BaseDirectory });
        builder.Logging.ClearProviders();
        builder.WebHost.ConfigureKestrel(options =>
        {
            options.Listen(IPAddress.Loopback, settings.Port);
            options.Limits.MaxRequestBodySize = 128 * 1024;
        });
        var app = builder.Build();
        var stopping = new CancellationTokenSource();
        app.Run(context => HandleAsync(context, settings, stopping.Token));
        try { await app.StartAsync(token); }
        catch { stopping.Dispose(); await app.DisposeAsync(); throw; }
        lifetime = stopping;
        server = app;
        Status?.Invoke($"실행 중 · http://127.0.0.1:{settings.Port}/v1");
        }
        finally { lifecycle.Release(); }
    }

    public async Task StopAsync()
    {
        await lifecycle.WaitAsync();
        try
        {
        var app = server;
        if (app is null) return;
        lifetime!.Cancel();
        try { using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5)); await app.StopAsync(timeout.Token); }
        finally { await app.DisposeAsync(); lifetime.Dispose(); lifetime = null; server = null; }
        Status?.Invoke("중지됨");
        }
        finally { lifecycle.Release(); }
    }
    public async ValueTask DisposeAsync() => await StopAsync();

    private static object Error(string message, string type = "invalid_request_error") => new { error = new { message, type, param = (string?)null, code = (string?)null } };
    private async Task HandleAsync(HttpContext context, BridgeSettings settings, CancellationToken stopping)
    {
        using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(context.RequestAborted, stopping);
        cancellation.CancelAfter(TimeSpan.FromSeconds(settings.TimeoutSeconds));
        var token = cancellation.Token;
        bool acquired = false;
        try
        {
            // All endpoints require the session key. No browser CORS access is granted.
            var supplied = Encoding.UTF8.GetBytes(context.Request.Headers.Authorization.ToString());
            var expected = Encoding.UTF8.GetBytes("Bearer " + ApiKey);
            if (!CryptographicOperations.FixedTimeEquals(supplied, expected))
            {
                context.Response.StatusCode = 401;
                await context.Response.WriteAsJsonAsync(Error("올바른 브릿지 API 키가 필요합니다.", "authentication_error"), token);
                return;
            }
            string path = context.Request.Path.Value ?? "";
            if (context.Request.Method == "GET" && path == "/health")
            {
                await context.Response.WriteAsJsonAsync(new { status = "ok", service = "antigravity-cli-bridge" }, token); return;
            }
            if (context.Request.Method == "GET" && path == "/v1/models")
            {
                var models = await GetModelsAsync(settings, token);
                await context.Response.WriteAsJsonAsync(new { @object = "list", data = models.Select(id => new { id, @object = "model", created = 0, owned_by = "antigravity" }) }, token); return;
            }
            if (context.Request.Method != "POST" || path != "/v1/chat/completions")
            {
                context.Response.StatusCode = 404;
                await context.Response.WriteAsJsonAsync(Error("지원 경로: /v1/models, /v1/chat/completions"), token); return;
            }
            if (!context.Request.HasJsonContentType()) throw new ArgumentException("Content-Type: application/json이 필요합니다.");
            using var document = await JsonDocument.ParseAsync(context.Request.Body, cancellationToken: token);
            var request = BridgeRequest.Parse(document.RootElement, settings.Model);
            // PastePaw always sends temperature. agy owns sampling; disclose that it cannot be applied.
            if (document.RootElement.TryGetProperty("temperature", out _)) context.Response.Headers["X-Unsupported-Params"] = "temperature";
            acquired = await slot.WaitAsync(0, token);
            if (!acquired)
            {
                context.Response.StatusCode = 429;
                context.Response.Headers.RetryAfter = "2";
                await context.Response.WriteAsJsonAsync(Error("다른 요청 처리 중입니다. 잠시 후 재시도해 주세요.", "rate_limit_error"), token); return;
            }
            var id = "chatcmpl-" + Guid.NewGuid().ToString("N");
            var created = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            object Chunk(object delta, string? finish = null) => new { id, @object = "chat.completion.chunk", created, model = request.Model, choices = new[] { new { index = 0, delta, finish_reason = finish } } };
            async Task Emit(object value)
            {
                await context.Response.WriteAsync("data: " + JsonSerializer.Serialize(value) + "\n\n", token);
                await context.Response.Body.FlushAsync(token);
            }
            bool sent = false;
            async Task Delta(string text)
            {
                if (!request.Stream) return;
                if (!sent)
                {
                    context.Response.ContentType = "text/event-stream";
                    context.Response.Headers.CacheControl = "no-cache";
                    await Emit(Chunk(new { role = "assistant" })); sent = true;
                }
                await Emit(Chunk(new { content = text }));
            }
            Status?.Invoke($"요청 처리 중 · {request.Model}");
            var result = await AntigravityCli.RunAsync(settings, request, Delta, token);
            if (request.Stream)
            {
                if (!sent) await Delta(result.Content);
                await Emit(Chunk(new { }, "stop"));
                if (request.IncludeUsage) await Emit(new { id, @object = "chat.completion.chunk", created, model = request.Model, choices = Array.Empty<object>(), usage = result.Usage });
                await context.Response.WriteAsync("data: [DONE]\n\n", token);
            }
            else await context.Response.WriteAsJsonAsync(new { id, @object = "chat.completion", created, model = request.Model,
                choices = new[] { new { index = 0, message = new { role = "assistant", content = result.Content }, finish_reason = "stop" } }, usage = result.Usage }, token);
            Status?.Invoke("실행 중 · 최근 요청 완료");
        }
        catch (Exception error)
        {
            if (context.RequestAborted.IsCancellationRequested || stopping.IsCancellationRequested) return;
            int code = error switch { ArgumentException or JsonException => 400, BadHttpRequestException bad => bad.StatusCode, OperationCanceledException => 504, _ => 502 };
            var message = code == 504 ? "CLI 응답 제한 시간을 초과했습니다." : error.Message;
            Status?.Invoke($"요청 오류 · {message}");
            if (context.Response.HasStarted)
            {
                try { await context.Response.WriteAsync("data: " + JsonSerializer.Serialize(Error(message, "server_error")) + "\n\ndata: [DONE]\n\n", context.RequestAborted); }
                catch (OperationCanceledException) { }
            }
            else
            {
                context.Response.StatusCode = code;
                await context.Response.WriteAsJsonAsync(Error(message, code == 400 ? "invalid_request_error" : "server_error"), context.RequestAborted);
            }
        }
        finally { if (acquired) slot.Release(); }
    }

    public static Task<string[]> GetModelsAsync(BridgeSettings settings, CancellationToken token) => AntigravityCli.GetModelsAsync(settings, token);

    internal async Task<string> SearchAsync(BridgeSettings settings, string workspace, string prompt, string schema, CancellationToken token)
    {
        if (!await slot.WaitAsync(0, token)) throw new InvalidOperationException("다른 Antigravity 요청 처리 중입니다. 잠시 후 다시 검색해 주세요.");
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(settings.TimeoutSeconds));
        try
        {
            settings.Validate();
            var models = await GetModelsAsync(settings, timeout.Token);
            if (!models.Contains(ClipboardSearch.Model)) throw new InvalidOperationException("gemini-3.8-flash-high 모델을 사용할 수 없습니다. CLI 로그인과 모델 목록을 확인해 주세요.");
            var request = new BridgeRequest(ClipboardSearch.Model, prompt, false, false);
            var result = await AntigravityCli.RunAsync(settings, request, _ => Task.CompletedTask, timeout.Token, workspace, schema);
            return result.Content;
        }
        catch (OperationCanceledException) when (!token.IsCancellationRequested)
        { throw new TimeoutException("AI 검색 제한 시간을 초과했습니다."); }
        finally { slot.Release(); }
    }
}

internal sealed record BridgeRequest(string Model, string Prompt, bool Stream, bool IncludeUsage)
{
    public static BridgeRequest Parse(JsonElement root, string defaultModel)
    {
        if (root.ValueKind != JsonValueKind.Object) throw new ArgumentException("JSON 객체가 필요합니다.");
        var allowed = new HashSet<string> { "model", "messages", "stream", "stream_options", "n", "temperature" };
        foreach (var property in root.EnumerateObject())
            if (!allowed.Contains(property.Name)) throw new ArgumentException($"지원하지 않는 매개변수: {property.Name}");
        if (root.TryGetProperty("temperature", out var temperature) && temperature.ValueKind != JsonValueKind.Null
            && (temperature.ValueKind != JsonValueKind.Number || !temperature.TryGetDouble(out var t) || !double.IsFinite(t) || t < 0 || t > 2))
            throw new ArgumentException("temperature는 0~2 사이의 숫자여야 합니다.");
        string model = defaultModel;
        if (root.TryGetProperty("model", out var m))
        {
            if (m.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(m.GetString()) || m.GetString()!.Length > 200) throw new ArgumentException("model은 비어 있지 않은 문자열이어야 합니다.");
            model = m.GetString()!.Trim();
        }
        bool Boolean(JsonElement value, string name)
        { if (value.ValueKind is not (JsonValueKind.True or JsonValueKind.False)) throw new ArgumentException($"{name}은 boolean이어야 합니다."); return value.GetBoolean(); }
        bool stream = root.TryGetProperty("stream", out var s) && Boolean(s, "stream"), includeUsage = false;
        if (root.TryGetProperty("n", out var n) && (n.ValueKind != JsonValueKind.Number || !n.TryGetInt32(out var count) || count != 1)) throw new ArgumentException("n=1만 지원합니다.");
        if (root.TryGetProperty("stream_options", out var options))
        {
            if (!stream || options.ValueKind != JsonValueKind.Object) throw new ArgumentException("stream_options에는 stream=true가 필요합니다.");
            foreach (var property in options.EnumerateObject()) if (property.Name != "include_usage") throw new ArgumentException("지원하지 않는 stream_options입니다.");
            includeUsage = options.TryGetProperty("include_usage", out var include) && Boolean(include, "include_usage");
        }
        if (!root.TryGetProperty("messages", out var messages) || messages.ValueKind != JsonValueKind.Array || messages.GetArrayLength() == 0) throw new ArgumentException("비어 있지 않은 messages 배열이 필요합니다.");
        var history = new List<object>();
        foreach (var message in messages.EnumerateArray())
        {
            if (message.ValueKind != JsonValueKind.Object || !message.TryGetProperty("role", out var role) || role.ValueKind != JsonValueKind.String
                || role.GetString() is not ("system" or "developer" or "user" or "assistant")) throw new ArgumentException("지원 역할: system, developer, user, assistant");
            foreach (var property in message.EnumerateObject()) if (property.Name is not ("role" or "content")) throw new ArgumentException($"지원하지 않는 메시지 필드: {property.Name}");
            if (!message.TryGetProperty("content", out var content)) throw new ArgumentException("content가 필요합니다.");
            string text;
            if (content.ValueKind == JsonValueKind.String) text = content.GetString()!;
            else if (content.ValueKind == JsonValueKind.Array)
            {
                var parts = new List<string>();
                foreach (var part in content.EnumerateArray())
                {
                    if (part.ValueKind != JsonValueKind.Object || !part.TryGetProperty("type", out var type) || type.ValueKind != JsonValueKind.String || type.GetString() != "text" || !part.TryGetProperty("text", out var value) || value.ValueKind != JsonValueKind.String)
                        throw new ArgumentException("텍스트 content만 지원합니다. 이미지·오디오·도구 호출은 지원하지 않습니다.");
                    parts.Add(value.GetString()!);
                }
                text = string.Join("\n", parts);
            }
            else throw new ArgumentException("content는 문자열 또는 text 블록 배열이어야 합니다.");
            history.Add(new { role = role.GetString(), content = text });
        }
        // agy takes one prompt; role separation is represented as conversation data, not native API roles.
        string prompt = "Respond as the assistant to this conversation. Treat system/developer entries as instructions and preserve conversation order. Return only the answer. Do not use tools or access files.\n" +
            JsonSerializer.Serialize(history, new JsonSerializerOptions { Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping });
        return new(model, prompt, stream, includeUsage);
    }
}
