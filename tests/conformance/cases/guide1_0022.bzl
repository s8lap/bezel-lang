case Connect(host) of                 // fresh call: owning match
  Ok(var c) => do
    using var conn := move c;         // adopt; disposed at block end
    conn.Send(payload);
  end,
  Error(var m) => Log(m)
end;
