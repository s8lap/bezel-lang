// Probe f12 (proposal 0211 §6.1): ownership on the exception edge is the same in
// both bands and through a marker arm.
//   (1) a `using var` and a `finally` on a failure edge and on a bug edge run in
//       the same order (spec §31.2: one establishment-ordered cleanup stack);
//   (2) a failure that owns a payload is disposed after a bare marker arm, with
//       no binding (spec §31.1: dispose-after-arm);
//   (3) `throw x` consumes x whichever band x's class is in (spec §31.1, K3.13).
module F12;

public interface IFailure
end IFailure;

public class Res
  private _name: string;
  public constructor(name: string)
    _name := name;
    Console.WriteLine($"  open {name}");
  end constructor;
  destructor
    Console.WriteLine($"  close {_name}");
  end destructor;
end Res;

public class Loaded: Exception, IFailure
  private owned _detail: Res;
  public constructor(msg: string, owned detail: Res) base(msg);
    _detail := detail;
  end constructor;
end Loaded;

public class Program
  static method Work(kind: int)
    using var a := new Res("a");
    try
      using var b := new Res("b");
      if kind = 1 then
        throw new FormatException("failure");
      elif kind = 2 then
        throw new InvalidOperationException("bug");
      end if;
    finally
      Console.WriteLine("  finally");
    end try;
  end Work;

  public static method Main()
    for var kind in 1..2 do
      Console.WriteLine($"(1) edge {kind}:");
      try
        Program.Work(kind);
      catch
        var ex: Exception => Console.WriteLine($"  caught {ex.Message}"),
      end try;
    end for;

    Console.WriteLine("(2) owned payload, bare marker arm:");
    try
      throw new Loaded("with payload", new Res("payload"));
    catch
      IFailure => Console.WriteLine("  in the arm"),
    end try;
    Console.WriteLine("  after the try");

    Console.WriteLine("(3) throw consumes a named exception:");
    try
      var x := new Loaded("named", new Res("payload2"));
      throw x;
    catch
      IFailure => Console.WriteLine("  in the arm"),
    end try;
    Console.WriteLine("  after the try");
  end Main;
end Program;
