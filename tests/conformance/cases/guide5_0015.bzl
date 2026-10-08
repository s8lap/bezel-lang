for var g in requests.GroupBy(lambda(r) => r.Route) do     // outer: consumed by this loop
  Console.WriteLine(
    $"{g.Key}: {g.Count()} calls, slowest {g.Max(lambda(r) => r.Millis)} ms");
end;                                                       // g read twice — defined
