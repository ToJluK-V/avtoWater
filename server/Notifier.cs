using System.Text;
using System.Text.Json;

namespace AvtoWater.Server;

// Обновляет одно сообщение на устройство и периодически шлёт графики.
public class Notifier
{
    const int ChartPoints = 48;

    readonly StateStore _store;
    readonly TelegramService _tg;
    readonly HttpClient _http;
    readonly ILogger<Notifier> _log;
    readonly SemaphoreSlim _gate = new(1, 1);
    readonly TimeSpan _tz;
    readonly TimeSpan _chartsInterval;

    public Notifier(StateStore store, TelegramService tg, HttpClient http,
                    IConfiguration cfg, ILogger<Notifier> log)
    {
        _store = store;
        _tg = tg;
        _http = http;
        _log = log;
        _tz = TimeSpan.FromHours(cfg.GetValue("TZ_OFFSET_HOURS", 3));
        _chartsInterval = TimeSpan.FromMinutes(cfg.GetValue("CHARTS_INTERVAL_MIN", 30));
    }

    string Fmt(DateTimeOffset? t) =>
        t is null ? "нет данных" : t.Value.ToOffset(_tz).ToString("dd.MM.yyyy HH:mm") + $" UTC{_tz.Hours:+0;-0}";

    string BuildStatus(DeviceState s)
    {
        var c = s.Config;
        var sb = new StringBuilder();

        sb.Append($"🌱 АВТОПОЛИВ: {s.Id}\n\n");
        sb.Append("🌡 Температура: ")
          .Append(s.Temperature is double t ? $"{t:0.0} °C" : "ошибка датчика").Append('\n');
        sb.Append("💧 Влажность (ADC): ")
          .Append(s.Soil is int soil ? soil.ToString() : "ошибка").Append('\n');
        sb.Append($"🎯 Порог полива: {c.Threshold}\n\n");
        sb.Append($"🪴 Последний полив: {Fmt(s.LastWateringAt)}\n");

        if (s.LastWateringMs is int ms)
        {
            sb.Append($"⏱ Длительность: {ms / 1000.0:0.#} с\n");
        }

        if (s.WateredLastReport && s.PostWaterSoil is int post)
        {
            sb.Append($"🌱 После полива (ADC): {post}\n");
        }

        sb.Append($"\n🕒 Обновление: {Fmt(s.LastSeen)}\n");

        if (s.Soil is int v)
        {
            var dry = c.Invert ? v > c.Threshold : v < c.Threshold;
            sb.Append(dry ? "⚠️ Почва сухая" : "✅ Влажность в норме");
        }
        else
        {
            sb.Append("⚠️ Нет показаний датчика");
        }

        return sb.ToString();
    }

    public async Task NotifyAsync(string deviceId)
    {
        if (!_tg.Enabled) return;

        await _gate.WaitAsync();
        try
        {
            var s = _store.Clone(deviceId);
            var text = BuildStatus(s);

            var ok = s.TelegramMessageId is long mid && await _tg.EditAsync(mid, text);

            if (!ok && await _tg.SendAsync(text) is long newId)
            {
                _store.With(deviceId, d => { d.TelegramMessageId = newId; return 0; });
            }

            var due = s.LastChartsAt is null ||
                      DateTimeOffset.UtcNow - s.LastChartsAt >= _chartsInterval - TimeSpan.FromMinutes(5);

            if (due && s.History.Count >= 2 && await SendChartsAsync(s))
            {
                _store.With(deviceId, d => { d.LastChartsAt = DateTimeOffset.UtcNow; return 0; });
            }
        }
        catch (Exception ex)
        {
            _log.LogWarning(ex, "Telegram update failed for {Device}", deviceId);
        }
        finally
        {
            _gate.Release();
        }
    }

    async Task<bool> SendChartsAsync(DeviceState s)
    {
        var points = s.History.TakeLast(ChartPoints).ToList();
        var labels = points.Select(p => p.At.ToOffset(_tz).ToString("HH:mm")).ToArray();

        var temp = await RenderChartAsync(labels, points.Select(p => p.Temperature).ToArray(),
                                          "Temperature (C)", "#e74c3c");
        var soil = await RenderChartAsync(labels, points.Select(p => (double?)p.Soil).ToArray(),
                                          "Soil moisture (ADC)", "#3498db");

        var a = temp is not null &&
                await _tg.SendPhotoAsync(temp, $"{s.Id}: температура, последние измерения");
        var b = soil is not null &&
                await _tg.SendPhotoAsync(soil, $"{s.Id}: влажность почвы, последние измерения");

        return a && b;
    }

    async Task<byte[]?> RenderChartAsync(string[] labels, double?[] values, string title, string color)
    {
        var payload = new
        {
            version = "3",
            width = 640,
            height = 360,
            format = "png",
            backgroundColor = "white",
            chart = new
            {
                type = "line",
                data = new
                {
                    labels,
                    datasets = new[]
                    {
                        new { label = title, data = values, borderColor = color, fill = false, pointRadius = 2 }
                    }
                },
                options = new
                {
                    plugins = new
                    {
                        legend = new { display = false },
                        title = new { display = true, text = title }
                    },
                    scales = new { y = new { beginAtZero = false } }
                }
            }
        };

        var resp = await _http.PostAsync(
            "https://quickchart.io/chart",
            new StringContent(JsonSerializer.Serialize(payload), Encoding.UTF8, "application/json"));

        return resp.IsSuccessStatusCode ? await resp.Content.ReadAsByteArrayAsync() : null;
    }
}
