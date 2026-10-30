namespace AvtoWater.Server;

// Render усыпляет бесплатный сервис без входящих запросов, поэтому пингуем внешний адрес.
public class KeepAlive : BackgroundService
{
    readonly HttpClient _http;
    readonly string? _url;
    readonly TimeSpan _interval;
    readonly ILogger<KeepAlive> _log;

    public KeepAlive(HttpClient http, IConfiguration cfg, ILogger<KeepAlive> log)
    {
        _http = http;
        _log = log;

        var baseUrl = cfg["PUBLIC_URL"] ?? cfg["RENDER_EXTERNAL_URL"];
        _url = string.IsNullOrEmpty(baseUrl) ? null : baseUrl.TrimEnd('/') + "/healthz";
        _interval = TimeSpan.FromMinutes(cfg.GetValue("KEEPALIVE_INTERVAL_MIN", 10));
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        if (_url is null) return;

        using var timer = new PeriodicTimer(_interval);

        while (await timer.WaitForNextTickAsync(stoppingToken))
        {
            try
            {
                await _http.GetAsync(_url, stoppingToken);
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                _log.LogWarning(ex, "Keep-alive ping failed");
            }
        }
    }
}
