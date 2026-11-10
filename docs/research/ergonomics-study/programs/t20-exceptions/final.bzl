module Errors.Exceptions;

public class ConfigError: Exception
  public constructor(message: string, cause: Exception)
    base(message, cause);
  end constructor;
end ConfigError;

public static class Config
  public static method LoadPort(text: string): int
    try
      return int.Parse(text);
    catch
      var ex: FormatException => throw new ConfigError("invalid port", ex)
    end;
  end LoadPort;
end Config;

public class Program
  public static method Main()
    try
      var p := Config.LoadPort("80a");
      Console.WriteLine($"port {p}");
    catch
      var e: ConfigError => do
        Console.WriteLine($"error: {e.Message}");
        for var c in e.CauseChain do
          Console.WriteLine($"  caused by: {c.TypeName}");
        end;
      end
    finally
      Console.WriteLine("cleanup");
    end;
  end Main;
end Program;
