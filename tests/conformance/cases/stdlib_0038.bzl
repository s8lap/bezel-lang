// Value-payload pipelines over the combinators (value-like instantiations).
var port := ParsePort(raw).UnwrapOr(8080);

var label := ParsePort(raw)
    .Map(lambda(p) => $"listening on {p}")
    .UnwrapOr($"bad port: {raw}");

// Re-code the error at a boundary (MapError):
public record ConfigError(Key: string, Message: string);

public method PortSetting(raw: string): Result<int, ConfigError>
  return ParsePort(raw).MapError(lambda(m) => new ConfigError("port", m));
end PortSetting;

// Sequence two fallible steps without an intermediate case (AndThen); the
// ascribed lambda's returns are declared-result positions (spec §35.3):
public method WellKnownPort(text: string): Result<int, string>
  return ParsePort(text).AndThen(lambda(n): Result<int, string> => do
    if n > 1023 then return Error($"not a well-known port: {n}"); end;
    return Ok(n);
  end);
end WellKnownPort;
