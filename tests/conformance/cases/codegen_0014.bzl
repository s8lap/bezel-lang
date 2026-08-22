for var conn in pool.Drain() do
  using var c := move conn;      // adopt on the first line — covers every exit edge
  c.SendShutdown();
end;                             // pool is now an empty container, still disposable
