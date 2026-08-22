pool.Clear();                                                  // disposes every element through the owner
for var conn in pool.Drain() do using var c := move conn; ... end;
