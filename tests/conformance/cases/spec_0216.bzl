public method UseEach()
  using var pool := new OwningArray<Connection>();
  pool.Add(Connect("primary"));      // ownership transfers in
  pool.Add(Connect("secondary"));     // ownership transfers in

  // Default iteration: each conn is a borrow
  for var conn in pool do
    conn.Ping();                      // pool still owns conn afterward
  end;

  // Drain iteration: each conn is owned
  for var conn in pool.Drain() do
    using var c := conn;              // discharge each owned element
    c.SendShutdown();
  end;
  // pool is now empty; using var on pool disposes the (empty) array
end UseEach;
