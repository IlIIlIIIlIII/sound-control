using System.Runtime.InteropServices;
using System.Text.Json;

namespace SoundControl;
public sealed record AudioDevice(string Id, string Name)
{
    public override string ToString() => Name;
}
public sealed record Filter(double Frequency, double Gain, double Q)
{
    public string FrequencyText => $"{Frequency:0.##} Hz";
    public string GainText => $"{Gain:+0.##;-0.##;0} dB";
    public string QText => $"Q {Q:0.##}";
}
public sealed record Profile(bool Valid, string Summary, Filter[] Filters);
public sealed record Meter(string State, bool Active, int Rate, int Error, bool Enabled);
public sealed record AudioSnapshot(bool Ready, int Revision, bool EqEnabled, bool AecEnabled,
    string RenderId, string CaptureId, AudioDevice[] Outputs, AudioDevice[] Inputs,
    Profile Left, Profile Right, Meter Render, Meter Capture, bool Reference, bool Clipping, string? Error);

internal sealed class AudioService : IDisposable
{
    private readonly object gate = new();
    private nint handle = MT_Create();
    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true };
    public AudioService()
    {
        if (handle == 0) throw new InvalidOperationException("오디오 설정 서비스를 시작하지 못했습니다.");
    }
    public Task<AudioSnapshot> ReadAsync() => Task.Run(() =>
    {
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(handle == 0, this);
            var json = Marshal.PtrToStringUni(MT_Snapshot(handle)) ?? throw new InvalidDataException("빈 오디오 상태");
            var snapshot = JsonSerializer.Deserialize<AudioSnapshot>(json, JsonOptions)!;
            if (snapshot.Error is not null) throw new InvalidOperationException(snapshot.Error);
            return snapshot;
        }
    });
    public Task EnableAsync(bool eq, bool aec) => Task.Run(() =>
    {
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(handle == 0, this);
            Check(MT_SetEnabled(handle, eq ? 1 : 0, aec ? 1 : 0));
        }
    });
    public Task ImportAsync(int channel, string path) => Task.Run(() =>
    {
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(handle == 0, this);
            Check(MT_Import(handle, channel, path));
        }
    });
    private static void Check(nint result)
    {
        string error = Marshal.PtrToStringUni(result) ?? "오디오 서비스 응답 오류";
        if (error.Length > 0) throw new InvalidOperationException(error);
    }
    public void Dispose()
    {
        lock (gate) { if (handle != 0) MT_Destroy(handle); handle = 0; }
    }
    [DllImport("SoundControlBridge.dll", CallingConvention = CallingConvention.Cdecl)] private static extern nint MT_Create();
    [DllImport("SoundControlBridge.dll", CallingConvention = CallingConvention.Cdecl)] private static extern void MT_Destroy(nint handle);
    [DllImport("SoundControlBridge.dll", CallingConvention = CallingConvention.Cdecl)] private static extern nint MT_Snapshot(nint handle);
    [DllImport("SoundControlBridge.dll", CallingConvention = CallingConvention.Cdecl)] private static extern nint MT_SetEnabled(nint handle, int eq, int aec);
    [DllImport("SoundControlBridge.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern nint MT_Import(nint handle, int channel, string path);
}
