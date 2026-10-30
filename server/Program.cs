using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using AvtoWater.Server;

var builder = WebApplication.CreateBuilder(args);

// Render передаёт порт через PORT.
if (Environment.GetEnvironmentVariable("PORT") is string port)
{
    builder.WebHost.UseUrls($"http://0.0.0.0:{port}");
}

var deviceKey = builder.Configuration["DEVICE_KEY"]
    ?? throw new InvalidOperationException("Не задана переменная DEVICE_KEY.");
var adminKey = builder.Configuration["ADMIN_KEY"];

builder.Services.AddSingleton<StateStore>();
builder.Services.AddHttpClient<TelegramService>();
builder.Services.AddHttpClient<Notifier>();
builder.Services.AddHttpClient<KeepAlive>();
builder.Services.AddHostedService(sp => sp.GetRequiredService<KeepAlive>());

var app = builder.Build();

var idPattern = new Regex("^[A-Za-z0-9_-]{1,32}$");
var firmwareFile = Path.Combine(app.Services.GetRequiredService<StateStore>().DataDir, "firmware.bin");
var firmwareVersionFile = Path.Combine(Path.GetDirectoryName(firmwareFile)!, "firmware.version");

static bool KeyEquals(string? given, string? expected)
{
    if (string.IsNullOrEmpty(given) || string.IsNullOrEmpty(expected)) return false;

    return CryptographicOperations.FixedTimeEquals(
        SHA256.HashData(Encoding.UTF8.GetBytes(given)),
        SHA256.HashData(Encoding.UTF8.GetBytes(expected)));
}

bool IsDevice(HttpContext c) => KeyEquals(c.Request.Headers["X-Device-Key"], deviceKey);
bool IsAdmin(HttpContext c) => KeyEquals(c.Request.Headers["X-Admin-Key"], adminKey);

int FirmwareVersion() =>
    File.Exists(firmwareFile) && File.Exists(firmwareVersionFile) &&
    int.TryParse(File.ReadAllText(firmwareVersionFile).Trim(), out var v) ? v : 0;

app.MapGet("/healthz", () => "ok");

// Плата запрашивает настройки перед поливом.
app.MapGet("/api/v1/config", (HttpContext ctx, string deviceId, StateStore store) =>
{
    if (!IsDevice(ctx)) return Results.Unauthorized();
    if (!idPattern.IsMatch(deviceId)) return Results.BadRequest();

    var c = store.With(deviceId, s => s.Config);

    return Results.Json(new
    {
        threshold = c.Threshold,
        pumpMs = c.PumpMs,
        sleepMin = c.SleepMin,
        minWateringSec = c.MinWateringSec,
        invert = c.Invert ? 1 : 0,
        fwVersion = FirmwareVersion()
    });
});

// Плата присылает измерения и итог полива.
app.MapPost("/api/v1/report", (HttpContext ctx, ReportDto r, StateStore store, Notifier notifier) =>
{
    if (!IsDevice(ctx)) return Results.Unauthorized();
    if (!idPattern.IsMatch(r.DeviceId)) return Results.BadRequest();

    var now = DateTimeOffset.UtcNow;
    int? soil = r.Soil is >= 0 and <= 1023 ? r.Soil : null;
    var temp = r.Temperature is >= -55 and <= 125 ? r.Temperature : null;

    store.With(r.DeviceId, s =>
    {
        s.LastSeen = now;
        s.Temperature = temp;
        s.Soil = soil;
        s.PostWaterSoil = r.PostWaterSoil;
        s.Rssi = r.Rssi;
        s.FirmwareVersion = r.Fw;
        s.WateredLastReport = r.Watered;

        if (r.Watered)
        {
            s.LastWateringAt = now;
            s.LastWateringMs = s.Config.PumpMs;
        }

        if (soil is int v) s.History.Add(new Reading(now, temp, v));
        return 0;
    });

    // Плата не ждёт Telegram.
    _ = notifier.NotifyAsync(r.DeviceId);

    return Results.Ok(new { ok = true });
});

app.MapGet("/firmware.bin", (HttpContext ctx, string? key) =>
{
    if (!KeyEquals(key, deviceKey)) return Results.Unauthorized();
    if (!File.Exists(firmwareFile)) return Results.NotFound();

    return Results.File(firmwareFile, "application/octet-stream");
});

app.MapGet("/api/v1/devices", (HttpContext ctx, StateStore store) =>
    IsAdmin(ctx) ? Results.Json(store.All()) : Results.Unauthorized());

app.MapPut("/api/v1/devices/{id}/config", (HttpContext ctx, string id, DeviceConfig cfg, StateStore store) =>
{
    if (!IsAdmin(ctx)) return Results.Unauthorized();
    if (!idPattern.IsMatch(id)) return Results.BadRequest();

    cfg.Clamp();
    store.With(id, s => { s.Config = cfg; return 0; });
    return Results.Json(cfg);
});

// Загрузка новой прошивки: POST /api/v1/firmware?version=2, тело — .bin.
app.MapPost("/api/v1/firmware", async (HttpContext ctx, int version) =>
{
    if (!IsAdmin(ctx)) return Results.Unauthorized();
    if (version < 1) return Results.BadRequest();

    await using (var f = File.Create(firmwareFile))
    {
        await ctx.Request.Body.CopyToAsync(f);
    }

    await File.WriteAllTextAsync(firmwareVersionFile, version.ToString());
    return Results.Ok(new { version });
});

app.Run();
