public choice PollOutcome                          // an owning choice: Ready carries a resource
  Ready(owned conn: Connection, attempts: int),
  Empty;
end PollOutcome;

case pool.Poll() of                                // owning match over the fresh outcome
  Ready(move conn = var c, attempts = var n) => do // c: owned Connection; n: int copy
    using var live := move c;                      // adopt: scope-bound disposal
    Console.WriteLine($"connected after {n} tries");
  end,
  Empty => ()
end;
