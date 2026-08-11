// Declared-result positions: the declared type supplies Result<int, string> whole.
public method ParsePercent(text: string): Result<int, string>
  if not int.TryParse(text, out var value) then
    return Error($"not a number: {text}");   // = Result<int, string>.Error(…)
  end;
  if value < 0 or value > 100 then
    return Error($"out of range: {value}");
  end;
  return Ok(value);                          // = Result<int, string>.Ok(…)
end ParsePercent;

public method CachedOutcome(): FetchOutcome => NotModified;   // bare payload-free case value
