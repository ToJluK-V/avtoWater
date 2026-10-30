namespace AvtoWater.Server;

public class DeviceConfig
{
    public int Threshold { get; set; } = 150;
    public int PumpMs { get; set; } = 5000;
    public int SleepMin { get; set; } = 30;
    public int MinWateringSec { get; set; } = 6 * 60 * 60;
    // true: почва сухая, когда ADC выше порога.
    public bool Invert { get; set; }

    public void Clamp()
    {
        Threshold = Math.Clamp(Threshold, 1, 1023);
        PumpMs = Math.Clamp(PumpMs, 500, 30000);
        SleepMin = Math.Clamp(SleepMin, 1, 180);
        MinWateringSec = Math.Clamp(MinWateringSec, 60, 7 * 24 * 3600);
    }
}

public record Reading(DateTimeOffset At, double? Temperature, int Soil);

public class DeviceState
{
    public string Id { get; set; } = "";
    public DeviceConfig Config { get; set; } = new();
    public DateTimeOffset? LastSeen { get; set; }
    public double? Temperature { get; set; }
    public int? Soil { get; set; }
    public int? PostWaterSoil { get; set; }
    public int? Rssi { get; set; }
    public int? FirmwareVersion { get; set; }
    public DateTimeOffset? LastWateringAt { get; set; }
    public int? LastWateringMs { get; set; }
    public bool WateredLastReport { get; set; }
    public long? TelegramMessageId { get; set; }
    public DateTimeOffset? LastChartsAt { get; set; }
    public List<Reading> History { get; set; } = new();
}

public record ReportDto(
    string DeviceId,
    double? Temperature,
    int Soil,
    int? PostWaterSoil,
    bool Watered,
    int? Rssi,
    int? Fw);
