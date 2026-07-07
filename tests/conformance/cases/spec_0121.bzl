public class ConnectionPool
  // Try to fetch a cached connection; caller borrows from the pool.
  public method TryGetCached(name: string, out conn: Connection?): bool;

  // Try to fetch and remove from the pool; caller takes ownership.
  // §22.6 (v0.181): the borrow/take split uses DISTINCT names — the same-name
  // `out`/`out owned` pair is now a compile error (the v0.77 carve-out is
  // withdrawn), so the take-form is `TryTakeCached`, not a `TryGetCached`
  // overload.
  public method TryTakeCached(name: string, out owned conn: Connection?): bool;
end ConnectionPool;

// Caller A: borrows a cached connection (pool retains it)
if pool.TryGetCached("primary", out var c) then
  UseConnection(c!);    // c is Connection?; the bool guard tells us non-null
end;

// Caller B: takes ownership of a cached connection (pool no longer holds it)
if pool.TryTakeCached("primary", out using var c) then
  UseConnection(c!);    // c is owned Connection?; disposed at scope end
end;
