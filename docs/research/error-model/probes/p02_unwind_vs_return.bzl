// Probe p02: the throw edge and the return edge run the same cleanup, in the
// same order (spec §31.2 single establishment-ordered cleanup stack, §28.10,
// K3.15). This is the property a value-return implementation of declared
// failures would rely on.
module P02;

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

public class Program
  public static method Work(fail: bool, early: bool): int
    using var a := new Res("a");
    try
      using var b := new Res("b");
      var c := new Res("c");            // plain owned local: auto-dropped at scope end
      if fail then
        throw new InvalidOperationException("boom");
      end if;
      if early then
        return 1;
      end if;
      Console.WriteLine("  body done");
      c.Dispose();
    finally
      Console.WriteLine("  finally");
    end try;
    using var d := new Res("d");
    return 0;
  end Work;

  public static method Main()
    Console.WriteLine("normal:");
    _ := Work(false, false);
    Console.WriteLine("early return:");
    _ := Work(false, true);
    Console.WriteLine("throw:");
    try
      _ := Work(true, false);
    catch
      var ex: InvalidOperationException => Console.WriteLine($"  caught {ex.Message}"),
    end try;
  end Main;
end Program;
