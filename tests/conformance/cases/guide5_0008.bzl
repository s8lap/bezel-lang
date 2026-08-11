for var conn in pool.Drain() do
  using var c := move conn;      // adopt: yours for this iteration, disposed at body end
  c.SendShutdown();
end;
// pool is now empty
