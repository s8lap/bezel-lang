// Probe f09 (proposal 0211 §6.3): today the band does not survive a domain join.
// A failure and a bug raised in a spawned domain both arrive as
// `DomainFaultedException`; an arm on the marker matches neither.
module F09;

import System.Threading;

public interface IFailure
end IFailure;

public class BadInput: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInput;

public class Program
  public static method Fails(): int
    throw new BadInput("bad input in the child");
  end Fails;

  public static method Bug(): int
    var xs := new Array<int>(2, 0);
    return xs[5];
  end Bug;

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.Fails);
    try
      var n := await t;
      Console.WriteLine(n);
    catch
      IFailure => Console.WriteLine("failure: matched the marker arm"),
      var ex: DomainFaultedException => Console.WriteLine($"failure: arrived as DomainFaultedException only ('{ex.Message}')"),
    end try;

    using var u := Domain.Spawn(Program.Bug);
    try
      var n := await u;
      Console.WriteLine(n);
    catch
      IFailure => Console.WriteLine("bug: matched the marker arm"),
      var ex: DomainFaultedException => Console.WriteLine($"bug: arrived as DomainFaultedException only ('{ex.Message}')"),
    end try;
  end Main;
end Program;
