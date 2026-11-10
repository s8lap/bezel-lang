// Probe f19 (proposal 0211 §5.5): the idioms the guide would teach, with the
// marker declared by the program (r40 has no built-in marker).
//   (1) a failure class declared with a primary constructor;
//   (2) a boundary: failures answered, cancellation re-raised, defects reported;
//   (3) both kinds in one arm on purpose: `IFailure | Exception`;
//   (4) deliberate suppression of failures only: `IFailure => ()`.
module F19;

public interface IFailure
end IFailure;

public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;

public class Program
  static method Load(kind: int): int
    if kind = 1 then
      throw new ConfigError("port is not a number");
    elif kind = 2 then
      using var xs := new Array<int>(1, 0);
      return xs[4];
    elif kind = 3 then
      throw new OperationCanceledException("shutting down");
    end if;
    return 8080;
  end Load;

  static method Handle(kind: int): string
    try
      return $"200 port {Program.Load(kind)}";
    catch
      IFailure => do return "400 bad configuration"; end,
      OperationCanceledException => throw,
      var ex: Exception => do
        Console.WriteLine($"  defect reported: {ex.ToString()}");
        return "500 internal error";
      end,
    end try;
  end Handle;

  public static method Main()
    for var kind in 0..3 do
      try
        Console.WriteLine($"(2) request {kind}: {Program.Handle(kind)}");
      catch
        OperationCanceledException => Console.WriteLine($"(2) request {kind}: cancellation passed the boundary"),
      end try;
    end for;

    for var kind in 1..2 do
      try
        _ := Program.Load(kind);
      catch
        var ex: IFailure | Exception => Console.WriteLine($"(3) one arm, both kinds: '{ex.Message}'"),
      end try;
    end for;

    try
      _ := Program.Load(1);
    catch
      IFailure => (),
    end try;
    Console.WriteLine("(4) failure suppressed; a defect would have propagated");
  end Main;
end Program;
