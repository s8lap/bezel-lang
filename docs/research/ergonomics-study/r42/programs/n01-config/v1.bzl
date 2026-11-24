module Tools.Config;

public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;

public class Config
  private owned _values: Dictionary<string, string> := new Dictionary<string, string>();

  public static method Parse(text: string): owned Config
    var cfg := new Config();
    for var raw in text.Split('\n') do
      var line := raw.Trim();
      if line.IsEmpty or line.StartsWith("#") then continue; end;
      var eq := line.IndexOf('=');
      if eq < 0 then
        throw new ConfigError($"bad line: {line}");
      end;
      cfg._values[line.Substring(0, eq).Trim()] := line.Substring(eq + 1).Trim();
    end;
    return cfg;
  end Parse;

  // the throwing form: this setting is required
  public method RequirePort(): int
    var raw := _values.GetOrNull("port");
    if raw = null then
      throw new ConfigError("missing port");
    end;
    var port := int.ParseOrNull(raw);
    if port = null then
      throw new ConfigError($"not a port: {raw}");
    end;
    return port;
  end RequirePort;

  // the non-throwing form: optional setting with a default
  public method WorkersOr(fallback: int): int => int.ParseOrNull(_values.GetOrNull("workers") ?? "") ?? fallback;
end Config;

public class Program
  private static method Show(name: string, text: string)
    using var cfg := Config.Parse(text);
    Console.WriteLine($"{name}: port={cfg.RequirePort()} workers={cfg.WorkersOr(4)}");
  end Show;

  public static method Main(): int
    try
      Show("A", "port = 8080\nworkers = x\n");
      Show("B", "workers = 2\n");
    catch
      var e: ConfigError => do
        Console.WriteLine($"B: configuration: {e.Message}");
        return 2;
      end
    end try;
    return 0;
  end Main;
end Program;
