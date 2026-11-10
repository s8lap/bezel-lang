// Probe p06: Result<T, E> with a value-like payload today (stdlib §2.20,
// spec §35). Shows the statement-position `case` each propagating call pays
// (proposal 0061 §2/§3), and what the combinators already remove.
module P06;

public record ConfigError(Key: string, Message: string);
public record Config(Host: string, Port: int);

public class Program
  public static method ParsePort(text: string): Result<int, ConfigError>
    if not int.TryParse(text, out var value) then
      return Error(new ConfigError("port", $"not a number: {text}"));
    end if;
    if value < 1 or value > 65535 then
      return Error(new ConfigError("port", $"out of range: {value}"));
    end if;
    return Ok(value);
  end ParsePort;

  public static method RequireHost(text: string): Result<string, ConfigError>
    if text.Length = 0 then
      return Error(new ConfigError("host", "empty"));
    end if;
    return Ok(text);
  end RequireHost;

  // Two fallible steps: one statement-case per step to propagate the error.
  public static method Load(host: string, port: string): Result<Config, ConfigError>
    var h := "";
    case Program.RequireHost(host) of
      Ok(var v)    => h := v;,
      Error(var e) => return Error(e);
    end case;
    var p := 0;
    case Program.ParsePort(port) of
      Ok(var v)    => p := v;,
      Error(var e) => return Error(e);
    end case;
    return Ok(new Config(h, p));
  end Load;

  // The same with the value-payload combinators (no new control flow).
  public static method Load2(host: string, port: string): Result<Config, ConfigError>
    return Program.RequireHost(host).AndThen(lambda(h): Result<Config, ConfigError> =>
      Program.ParsePort(port).Map(lambda(p) => new Config(h, p)));
  end Load2;

  public static method Show(r: Result<Config, ConfigError>)
    case r of
      Ok(var c)    => Console.WriteLine($"ok {c.Host}:{c.Port}"),
      Error(var e) => Console.WriteLine($"error {e.Key}: {e.Message}")
    end case;
  end Show;

  public static method Main()
    Program.Show(Program.Load("db", "5432"));
    Program.Show(Program.Load("db", "70000"));
    Program.Show(Program.Load2("", "5432"));
    Program.Show(Program.Load2("db", "x"));
    Console.WriteLine(Program.ParsePort("x").UnwrapOr(8080));
  end Main;
end Program;
