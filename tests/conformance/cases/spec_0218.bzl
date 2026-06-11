public method ReportEach(items: IEnumerable<Connection>)
  for var c in items do Console.WriteLine(c.Name); end;
end ReportEach;

public method UseOwnedPool()
  using var pool := new OwnedArray<Connection>();
  pool.Add(Connect("a"));
  pool.Add(Connect("b"));
  ReportEach(pool);     // OK: OwnedArray<Connection> implements IEnumerable<Connection>
                        //     iteration yields borrows
end UseOwnedPool;
