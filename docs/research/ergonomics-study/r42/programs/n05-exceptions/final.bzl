module Store.Errors;

// r42: `IFailure` is an unresolved type name (#8414)
// a base that subclasses wrap through needs both constructors, so no primary constructor (§11.1)
public class StorageError: Exception
  public constructor(message: string)
    base(message);
  end constructor;
  public constructor(message: string, cause: Exception)
    base(message, cause);
  end constructor;
end StorageError;

public class NotFoundError(key: string): StorageError($"not found: {key}")
  public readonly property Key: string := key;
end NotFoundError;

public class CorruptRecordError: StorageError
  public constructor(message: string, cause: Exception)
    base(message, cause);
  end constructor;
end CorruptRecordError;

public class Store
  private owned _data: Dictionary<string, string> := new Dictionary<string, string>();

  public method Put(key: string, value: string) => _data[key] := value;

  // the throwing form
  public method Get(key: string): int
    var raw := _data.GetOrNull(key);
    if raw = null then
      throw new NotFoundError(key);
    end;
    try
      return int.Parse(raw);
    catch
      var e: FormatException => throw new CorruptRecordError($"corrupt record {key}", e)
    end;
  end Get;

  // the non-throwing dual for callers that expect absence
  public method GetOrNull(key: string): int?
    var raw := _data.GetOrNull(key);
    return (if raw = null then null else int.ParseOrNull(raw));
  end GetOrNull;
end Store;

public class Program
  public static method Main()
    using var s := new Store();
    s.Put("a", "1");
    s.Put("c", "x1");
    Console.WriteLine($"a = {s.Get("a")}");
    Console.WriteLine($"b -> {s.GetOrNull("b")?.ToString() ?? "(absent)"}");
    var keys: Array<string> := ["b", "c"];
    for var k in keys do
      try
        Console.WriteLine(s.Get(k));
      catch
        var nf: NotFoundError => Console.WriteLine($"get {k}: {nf.Message}"),
        var se: StorageError => do
          Console.WriteLine($"get {k}: {se.Message}");
          for var c in se.CauseChain do
            Console.WriteLine($"  caused by {c.TypeName}");
          end;
        end
      end;
    end;
  end Main;
end Program;
