public method ReportEach(items: IEnumerable<Connection>)
  for var c in items do Console.WriteLine(c.Name); end;
end ReportEach;

public method UseOwnedPool()
  using var pool := new OwningArray<Connection>();
  pool.Add(Connect("a"));
  pool.Add(Connect("b"));
  ReportEach(pool);     // OK: OwningArray<Connection> implements IEnumerable<Connection>
                        //     iteration yields borrows
end UseOwnedPool;
