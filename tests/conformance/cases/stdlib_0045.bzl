// Borrowing key-iteration, one value taken at a time:
public method DrainAndProcess(dict: OwnedDictionary<string, Connection>)
  using var keys := new Array<string>(dict.Count);
  for var key in dict.Keys do
    keys.Add(key);
  end;
  for var key in keys do
    using var conn := dict.PopValue(key);     // takes ownership
    Process(conn);
  end;
  // dict is now empty
end DrainAndProcess;

// Single-call drain — each entry owns its value:
public method DrainAndProcess2(dict: OwnedDictionary<string, Connection>)
  for var entry in dict.Drain() do
    using var e := entry;                      // entry is owned; bind to discharge
    Process(e.Key, e.Value);                   // e.Value is the owned Connection
  end;
  // dict is now empty; each entry (and its Value) disposed at loop-body end
end DrainAndProcess2;
