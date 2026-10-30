using System.Text.Json;

namespace AvtoWater.Server;

public class StateStore
{
    const int MaxHistory = 500;

    readonly object _lock = new();
    readonly string _path;
    readonly Dictionary<string, DeviceState> _devices;

    public string DataDir { get; }

    public StateStore(IConfiguration cfg)
    {
        DataDir = cfg["DATA_DIR"] ?? "data";
        Directory.CreateDirectory(DataDir);
        _path = Path.Combine(DataDir, "state.json");

        _devices = File.Exists(_path)
            ? JsonSerializer.Deserialize<Dictionary<string, DeviceState>>(File.ReadAllText(_path)) ?? new()
            : new();
    }

    // Выполняет действие над состоянием устройства под блокировкой и сохраняет результат.
    public T With<T>(string id, Func<DeviceState, T> action)
    {
        lock (_lock)
        {
            if (!_devices.TryGetValue(id, out var state))
            {
                state = new DeviceState { Id = id };
                _devices[id] = state;
            }

            var result = action(state);

            if (state.History.Count > MaxHistory)
            {
                state.History.RemoveRange(0, state.History.Count - MaxHistory);
            }

            File.WriteAllText(_path, JsonSerializer.Serialize(_devices));
            return result;
        }
    }

    public DeviceState Clone(string id) => With(id, s =>
        JsonSerializer.Deserialize<DeviceState>(JsonSerializer.Serialize(s))!);

    public List<DeviceState> All()
    {
        lock (_lock)
        {
            return JsonSerializer.Deserialize<List<DeviceState>>(
                JsonSerializer.Serialize(_devices.Values))!;
        }
    }
}
