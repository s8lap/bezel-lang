// Probe p16: the fault/failure split written with what the language has today.
// A marker interface on exception classes and an interface-typed catch arm
// (spec §31.2.1 admits interface arm types). One arm takes every expected
// failure and lets every bug through; a derived class keeps an existing
// `catch OverflowException` working while joining the failure band.
module P16;

public interface IFailure
end IFailure;

public class BadInput: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInput;

public class RangeFailure: OverflowException, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end RangeFailure;

public class Program
  public static method Run(which: int): int
    using var xs := new Array<int>(2, 0);
    if which = 0 then
      throw new BadInput("not a number");
    elif which = 1 then
      throw new RangeFailure("too large");
    elif which = 2 then
      return xs[9];                       // bug
    end if;
    return 42;
  end Run;

  // What `(try Program.Run(which))` would mean: failures become null, bugs propagate.
  public static method RunOrNull(which: int): int?
    try
      return Program.Run(which);
    catch
      IFailure => return null;
    end try;
  end RunOrNull;

  public static method Main()
    for var i in 0..3 do
      try
        var r := Program.RunOrNull(i);
        if r = null then
          Console.WriteLine($"case {i}: null (a failure)");
        else
          Console.WriteLine($"case {i}: {r}");
        end if;
      catch
        var ex: Exception => Console.WriteLine($"case {i}: bug propagated past the failure arm: {ex.ToString()}"),
      end try;
    end for;
    // The derived class is still an OverflowException for existing arms.
    try
      _ := Program.Run(1);
    catch
      OverflowException => Console.WriteLine("existing `catch OverflowException` still matches RangeFailure"),
    end try;
  end Main;
end Program;
