// Owning instantiation (Extended): connect-or-explain.
public method Connect(host: string): Result<Connection, string>
  var conn := TryOpen(host);          // illustrative TryOpen(host): owned Connection?
  if conn = null then
    return Error($"unreachable: {host}");
  end;
  return Ok(move conn);                 // owned payload slot: marker consumes (spec §35.3)
end Connect;

case Connect(host) of                 // owning match over the temporary
  Ok(var c) => do
    using var conn := move c;         // adopt; disposed at block end
    conn.Send(payload);
  end,
  Error(var m) => Console.WriteLine($"giving up: {m}")
end;
