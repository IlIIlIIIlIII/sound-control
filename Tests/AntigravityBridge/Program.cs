using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using PersonalTools;

// This executable doubles as a deterministic agy fixture: exercise real subprocesses and HTTP.
if (args.Contains("models")) { Console.WriteLine("Fetching available models...\nfixture-model\tFixture Model"); return; }
if (args.Contains("--output-format"))
{
    string prompt = args[Array.IndexOf(args, "-p") + 1];
    if (prompt.Contains("EXIT_FAILURE")) { Environment.ExitCode = 7; return; }
    if (prompt.Contains("MISSING_RESULT")) return;
    if (prompt.Contains("SLOW_REQUEST"))
    {
        Console.WriteLine(JsonSerializer.Serialize(new { @event = "step_update", step_update = new { step_type = "agent_response", text_delta = "waiting" } }));
        await Task.Delay(TimeSpan.FromSeconds(60)); return;
    }
    var content = "한글 응답 \"quoted\" & | $()";
    Console.WriteLine(JsonSerializer.Serialize(new { @event = "step_update", step_update = new { step_type = "agent_response", text_delta = content } }));
    if (prompt.Contains("STREAM_FAILURE")) { Environment.ExitCode = 8; return; }
    Console.WriteLine(JsonSerializer.Serialize(new { @event = "result", result = new { status = "SUCCESS", response = content,
        usage = new { input_tokens = 12, output_tokens = 5, thinking_tokens = 2, cache_read_tokens = 3, total_tokens = 20 } } }));
    return;
}

int checks = 0;
void Check(bool condition, string name) { if (!condition) throw new Exception("FAIL: " + name); Console.WriteLine("PASS: " + name); checks++; }
int FreePort() { var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start(); int port = ((IPEndPoint)listener.LocalEndpoint).Port; listener.Stop(); return port; }
bool live = args.Contains("--live");
var settings = new BridgeSettings(live ? BridgeSettings.DefaultCli : Environment.ProcessPath!, FreePort(), TimeoutSeconds: live ? 60 : 10);
await using var bridge = new AntigravityBridge();
await bridge.StartAsync(settings);
using var client = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{settings.Port}"), Timeout = TimeSpan.FromSeconds(70) };
using (var unauthorized = await client.GetAsync("/v1/models")) Check(unauthorized.StatusCode == HttpStatusCode.Unauthorized, "API key required");
client.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", bridge.ApiKey);
using (var models = await client.GetAsync("/v1/models"))
{
    Check(models.IsSuccessStatusCode, "CLI model discovery");
    Check((await models.Content.ReadAsStringAsync()).Contains(live ? "gemini" : "fixture-model"), "actual model IDs");
}
object Body(string text, bool stream = false) => new { model = "antigravity", messages = new[] { new { role = "user", content = text } }, stream };
using (var response = await client.PostAsJsonAsync("/v1/chat/completions", Body(live ? "Reply exactly BRIDGE_OK. Do not use tools." : "hello \"한국어\" & | $()")))
{
    var body = await response.Content.ReadAsStringAsync();
    Check(response.IsSuccessStatusCode, "chat completion: " + response.StatusCode);
    using var json = JsonDocument.Parse(body);
    var content = json.RootElement.GetProperty("choices")[0].GetProperty("message").GetProperty("content").GetString();
    Check(content!.Contains(live ? "BRIDGE_OK" : "한글 응답"), "Unicode and answer preserved");
    Check(json.RootElement.GetProperty("usage").GetProperty("total_tokens").GetInt64() > 0, "reported token usage");
    if (!live) Check(json.RootElement.GetProperty("usage").GetProperty("completion_tokens").GetInt64() == 5, "thinking tokens not double counted");
}
using (var response = await client.PostAsJsonAsync("/v1/chat/completions", new {
    messages = new[] { new { role = "user", content = "Reply exactly STREAM_OK. Do not use tools." } }, stream = true, stream_options = new { include_usage = true } }))
{
    var body = await response.Content.ReadAsStringAsync();
    Check(response.IsSuccessStatusCode && response.Content.Headers.ContentType?.MediaType == "text/event-stream", "SSE content type");
    Check(body.Contains("chat.completion.chunk") && body.EndsWith("data: [DONE]\n\n"), "SSE framing and termination");
    Check(body.Contains("\"finish_reason\":\"stop\"") && body.Contains("\"choices\":[]"), "SSE finish and usage chunk");
    if (live) Check(body.Contains("STREAM_OK"), "live CLI streamed response");
}
if (!live)
{
    using (var pastePaw = await client.PostAsJsonAsync("/v1/chat/completions", new { model = "antigravity",
        messages = new[] { new { role = "system", content = "Summarize." }, new { role = "user", content = "Example." } }, temperature = 0.7 }))
    {
        Check(pastePaw.IsSuccessStatusCode, "PastePaw request compatibility");
        Check(pastePaw.Headers.GetValues("X-Unsupported-Params").Single() == "temperature", "unsupported sampling parameter disclosed");
    }
    using (var invalidTemperature = await client.PostAsJsonAsync("/v1/chat/completions", new { messages = new[] { new { role = "user", content = "hello" } }, temperature = 3 }))
        Check(invalidTemperature.StatusCode == HttpStatusCode.BadRequest, "invalid temperature rejected");
    await using (var collision = new AntigravityBridge())
    {
        bool failed = false;
        try { await collision.StartAsync(settings); } catch (IOException) { failed = true; }
        Check(failed && !collision.IsRunning, "occupied port does not report running");
    }
    foreach (var json in new[] { "{", "[]", "{\"messages\":[]}", "{\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}],\"tools\":[]}",
        "{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\"}]}]}",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"n\":\"bad\"}" })
    {
        using var bad = await client.PostAsync("/v1/chat/completions", new StringContent(json, Encoding.UTF8, "application/json"));
        Check(bad.StatusCode == HttpStatusCode.BadRequest, "reject malformed or unsupported request");
    }
    foreach (var message in new[] { "EXIT_FAILURE", "MISSING_RESULT" })
    {
        using var failed = await client.PostAsJsonAsync("/v1/chat/completions", Body(message));
        Check(failed.StatusCode == HttpStatusCode.BadGateway, "CLI failure mapped: " + message);
    }
    using (var failed = await client.PostAsJsonAsync("/v1/chat/completions", Body("STREAM_FAILURE", true)))
    {
        var body = await failed.Content.ReadAsStringAsync();
        Check(body.Contains("\"error\"") && !body.Contains("\"finish_reason\":\"stop\""), "stream failure is not successful finish");
    }
    using (var oversized = await client.PostAsJsonAsync("/v1/chat/completions", Body(new string('x', 20000))))
        Check(oversized.StatusCode == HttpStatusCode.BadRequest, "Windows argument size guard");
    using (var oversized = await client.PostAsJsonAsync("/v1/chat/completions", Body(new string('x', 150000))))
        Check(oversized.StatusCode == HttpStatusCode.RequestEntityTooLarge, "HTTP body size guard");
    using var slowMessage = new HttpRequestMessage(HttpMethod.Post, "/v1/chat/completions") { Content = JsonContent.Create(Body("SLOW_REQUEST", true)) };
    using (var slow = await client.SendAsync(slowMessage, HttpCompletionOption.ResponseHeadersRead))
    {
        using var busy = await client.PostAsJsonAsync("/v1/chat/completions", Body("hello"));
        Check(busy.StatusCode == HttpStatusCode.TooManyRequests, "bounded concurrency");
        var body = await slow.Content.ReadAsStringAsync();
        Check(body.Contains("server_error") && !body.Contains("\"finish_reason\":\"stop\""), "timeout terminates CLI and stream");
    }
    using var next = await client.PostAsJsonAsync("/v1/chat/completions", Body("hello"));
    Check(next.IsSuccessStatusCode, "slot recovered after timeout");
    using (var disconnect = new HttpRequestMessage(HttpMethod.Post, "/v1/chat/completions") { Content = JsonContent.Create(Body("SLOW_REQUEST", true)) })
    {
        using var cancellation = new CancellationTokenSource();
        var response = await client.SendAsync(disconnect, HttpCompletionOption.ResponseHeadersRead, cancellation.Token);
        cancellation.Cancel(); response.Dispose();
        bool recovered = false;
        for (int attempt = 0; attempt < 30; attempt++)
        {
            await Task.Delay(100);
            using var retry = await client.PostAsJsonAsync("/v1/chat/completions", Body("hello"));
            if (retry.IsSuccessStatusCode) { recovered = true; break; }
        }
        Check(recovered, "client disconnect cancels CLI and releases slot");
    }
    using var stopMessage = new HttpRequestMessage(HttpMethod.Post, "/v1/chat/completions") { Content = JsonContent.Create(Body("SLOW_REQUEST", true)) };
    using var pending = await client.SendAsync(stopMessage, HttpCompletionOption.ResponseHeadersRead);
    var watch = Stopwatch.StartNew();
    await bridge.StopAsync();
    Check(watch.Elapsed < TimeSpan.FromSeconds(8), "shutdown cancels active CLI");
    await bridge.StartAsync(settings);
    using var restarted = await client.GetAsync("/health");
    Check(restarted.IsSuccessStatusCode, "restart reuses port");
}
await bridge.StopAsync();
Check(!bridge.IsRunning, "server stopped");
Console.WriteLine($"{checks} checks passed ({(live ? "live Antigravity" : "deterministic subprocess")}).");
