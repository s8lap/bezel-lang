module Resources.Pool;

public class Connection(id: int)
  public readonly property Id: int := id;
  public method Query(sql: string)
    Console.WriteLine($"conn {Id}: {sql}");
  end Query;
  destructor
    Console.WriteLine($"close {Id}");
  end destructor;
end Connection;

public class Pool
  private owned _idle: OwningStack<Connection> := new OwningStack<Connection>();
  private _opened: int := 0;

  public method Lease(): owned Connection
    if _idle.Count > 0 then
      return _idle.Pop();
    end;
    _opened++;
    Console.WriteLine($"open {_opened}");
    return new Connection(_opened);
  end Lease;

  public method Return(owned c: Connection)
    _idle.Push(move c);
  end Return;

  public method With(action: (Connection) -> ())
    var c := Lease();
    try
      action(c);
    finally
      Return(move c);
    end;
  end With;
end Pool;

public class Program
  public static method Main()
    using var pool := new Pool();
    pool.With(lambda(c) => c.Query("query"));
    var a := pool.Lease();
    var b := pool.Lease();
    a.Query("a");
    b.Query("b");
    pool.Return(move a);
    pool.Return(move b);
    try
      pool.With(lambda(c) => do
        c.Query("will fail");
        throw new InvalidOperationException("boom");
      end);
    catch
      var ex: InvalidOperationException => Console.WriteLine($"caught {ex.Message}")
    end;
    Console.WriteLine("end of main");
  end Main;
end Program;
