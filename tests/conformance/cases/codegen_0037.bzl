public class Ports
  public method ParsePort(text: string): Result<int, string>
    if not int.TryParse(text, out var value) then
      return Error($"not a number: {text}");
    end;
    if not (value in 1..65535) then
      return Error($"out of range: {value}");
    end;
    return Ok(value);
  end ParsePort;

  public method Report(text: string)
    case ParsePort(text) of                    // exhaustive: no `_` arm needed
      Ok(var p)    => Console.WriteLine($"listening on {p}"),
      Error(var m) => Console.Error.WriteLine($"bad port: {m}")
    end;
  end Report;
end Ports;
