var port := ParsePort(raw).UnwrapOr(8080);           // fold: the value, or the fallback

var label := ParsePort(raw)
    .Map(lambda(p) => $"listening on {p}")
    .UnwrapOr($"bad port: {raw}");

// Re-code the error where a subsystem boundary wants its own error shape:
public record ConfigError(Key: string, Message: string);

public method PortSetting(raw: string): Result<int, ConfigError>
  return ParsePort(raw).MapError(lambda(m) => new ConfigError("port", m));
end PortSetting;

// Sequence two fallible steps without matching in between:
public method WellKnownPort(text: string): Result<int, string>
  return ParsePort(text).AndThen(lambda(n): Result<int, string> => do
    if n > 1023 then return Error($"not a well-known port: {n}"); end;
    return Ok(n);
  end);
end WellKnownPort;
