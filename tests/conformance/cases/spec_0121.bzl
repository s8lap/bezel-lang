public class ConnectionPool
  // Try to fetch a cached connection; caller borrows from the pool.
  public method TryGetCached(name: string, out conn: Connection?): bool;

  // Try to fetch and remove from the pool; caller takes ownership.
  public method TryGetCached(name: string, out owned conn: Connection?): bool;
end ConnectionPool;

// Caller A: borrows a cached connection (pool retains it)
if pool.TryGetCached("primary", out var c) then
  UseConnection(c!);    // c is Connection?; the bool guard tells us non-null
end;

// Caller B: takes ownership of a cached connection (pool no longer holds it)
if pool.TryGetCached("primary", out using var c) then
  UseConnection(c!);    // c is owned Connection?; disposed at scope end
end;
