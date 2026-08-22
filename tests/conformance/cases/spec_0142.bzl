public class ConnectionPool  // declaration sketch; method bodies elided
  // Borrow a cached connection; the pool retains ownership.
  public method TryGetCached(name: string, out conn: Connection? when true): bool;

  // Take a cached connection out of the pool; the caller assumes ownership.
  public method TryPopCached(name: string, out owned conn: Connection? when true): bool;
end ConnectionPool;

// Caller A: borrows a cached connection (pool retains it)
if pool.TryGetCached("primary", out var c) then
  UseConnection(c);     // narrowed non-null by the `when true` postcondition (§22.4)
end;

// Caller B: takes ownership of a cached connection (pool no longer holds it)
if pool.TryPopCached("primary", out using var c) then
  UseConnection(c);     // owned, narrowed non-null; disposed at scope end
end;
