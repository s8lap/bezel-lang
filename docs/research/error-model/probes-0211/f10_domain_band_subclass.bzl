// Probe f10 (proposal 0211 §6.3): the proposed join type, written today as a user
// class: a subclass of `DomainFaultedException` that implements the marker.
// Existing `catch DomainFaultedException` arms match it; a marker arm matches it;
// a plain `DomainFaultedException` (the bug case) matches the first and not the
// second. The runtime does not raise this class today; the probe throws it by hand.
module F10;

import System.Threading;

public interface IFailure
end IFailure;

public class DomainFailedException: DomainFaultedException, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end DomainFailedException;

public class Program
  static method Join(failure: bool)
    if failure then
      throw new DomainFailedException("bad input in the child");
    else
      throw new DomainFaultedException("index out of range in the child");
    end if;
  end Join;

  public static method Main()
    for var failure in [true, false] do
      try
        Program.Join(failure);
      catch
        IFailure => Console.WriteLine($"failure={failure}: marker arm"),
        DomainFaultedException => Console.WriteLine($"failure={failure}: DomainFaultedException arm only"),
      end try;
      try
        Program.Join(failure);
      catch
        var ex: DomainFaultedException => Console.WriteLine($"failure={failure}: an existing DomainFaultedException arm still matches ('{ex.Message}')"),
      end try;
    end for;
  end Main;
end Program;
