for var conn in pool.Drain() do
  using var c := move conn;       // adopt: disposed at end of this iteration
  c.Send(heartbeat);
end;
