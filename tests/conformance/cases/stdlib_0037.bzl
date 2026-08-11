// Value-like instantiation (Core): parse outcome with information.
public method ParsePort(text: string): Result<int, string>
  if not int.TryParse(text, out var value) then
    return Error($"not a number: {text}");    // declared-result position (spec §35.3)
  end;
  if value < 1 or value > 65535 then
    return Error($"out of range: {value}");
  end;
  return Ok(value);
end ParsePort;

case ParsePort(arg) of
  Ok(var p)    => Listen(p),
  Error(var m) => Console.WriteLine($"bad port: {m}")
end;                                  // exhaustive; no _ needed
