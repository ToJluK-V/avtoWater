using System.Net.Http.Json;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace AvtoWater.Server;

public class TelegramService
{
    readonly HttpClient _http;
    readonly string? _token;
    readonly string? _chatId;

    public bool Enabled => !string.IsNullOrEmpty(_token) && !string.IsNullOrEmpty(_chatId);

    public TelegramService(HttpClient http, IConfiguration cfg)
    {
        _http = http;
        _token = cfg["TELEGRAM_BOT_TOKEN"];
        _chatId = cfg["TELEGRAM_CHAT_ID"];
    }

    string Url(string method) => $"https://api.telegram.org/bot{_token}/{method}";

    public async Task<long?> SendAsync(string text)
    {
        var resp = await _http.PostAsJsonAsync(Url("sendMessage"), new { chat_id = _chatId, text });
        var json = JsonNode.Parse(await resp.Content.ReadAsStringAsync());
        return (long?)json?["result"]?["message_id"];
    }

    public async Task<bool> EditAsync(long messageId, string text)
    {
        var resp = await _http.PostAsJsonAsync(
            Url("editMessageText"),
            new { chat_id = _chatId, message_id = messageId, text });
        var body = await resp.Content.ReadAsStringAsync();

        return resp.IsSuccessStatusCode || body.Contains("message is not modified");
    }

    public async Task<bool> SendPhotoAsync(byte[] png, string caption)
    {
        using var form = new MultipartFormDataContent
        {
            { new StringContent(_chatId!), "chat_id" },
            { new StringContent(caption), "caption" },
            { new ByteArrayContent(png), "photo", "chart.png" }
        };

        var resp = await _http.PostAsync(Url("sendPhoto"), form);
        return resp.IsSuccessStatusCode;
    }
}
