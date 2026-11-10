// Probe f01 (proposal 0211 §3): the three marker shapes side by side, with what a
// boundary arm can do with the binding in each.
//   (A) empty interface      — binding reads ToString() only
//   (C) base class           — binding is an Exception: Message, pass as Exception, chain as cause
// Shape (B), an interface that declares Message, is f02 and f03.
module F01;

public interface IFailure
end IFailure;

public class Failure: Exception
  public constructor(msg: string) base(msg);
  end constructor;
end Failure;

public class BadInputI: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInputI;

public class BadInputC: Failure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInputC;

public class Wrapped: Exception
  public constructor(msg: string, cause: Exception) base(msg, cause);
  end constructor;
end Wrapped;

public class Program
  static method Log(ex: Exception)
    Console.WriteLine($"  Log(Exception): {ex.Message}");
  end Log;

  public static method Main()
    // (A) interface arm, binding at the interface type.
    try
      throw new BadInputI("bad digits");
    catch
      var f: IFailure => Console.WriteLine($"(A) interface binding: {f.ToString()}"),
    end try;

    // (A) the Exception view through a guard that does not narrow the binding.
    try
      throw new BadInputI("bad digits");
    catch
      var ex: Exception when (ex as IFailure) <> null => do
        Console.WriteLine($"(A) guard with `as`: Message = {ex.Message}");
        Program.Log(ex);
      end,
    end try;

    // (C) base-class arm: the binding is an Exception.
    try
      try
        throw new BadInputC("bad digits");
      catch
        var f: Failure => do
          Console.WriteLine($"(C) class binding: Message = {f.Message}");
          Program.Log(f);
          throw new Wrapped("lookup failed", f);
        end,
      end try;
    catch
      var w: Wrapped => Console.WriteLine($"(C) chained: {w.Message}; causes = {w.CauseChain.Count}"),
    end try;
  end Main;
end Program;
