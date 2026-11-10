// Probe f13 (proposal 0211 §4, §6): what a marker arm takes and what passes it.
// One boundary, six conditions. Expected: the two failures are taken; the
// contract violation, the index fault, the null fault and the cancellation pass.
module F13;

import System.IO;

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

public class Account
  private _balance: int;
  public method Withdraw(amount: int)
      requires(amount > 0)
    _balance -= amount;
  end Withdraw;
end Account;

public class Program
  static method Raise(which: int)
    if which = 0 then
      throw new BadInput("bad input");
    elif which = 1 then
      throw new RangeFailure("out of range");
    elif which = 2 then
      using var acct := new Account();
      acct.Withdraw(-5);
    elif which = 3 then
      using var xs := new Array<int>(2, 0);
      Console.WriteLine(xs[9]);
    elif which = 4 then
      var s: string? := null;
      Console.WriteLine(s!.Length);
    elif which = 5 then
      throw new OperationCanceledException("cancelled");
    end if;
  end Raise;

  public static method Main()
    for var i in 0..5 do
      try
        try
          Program.Raise(i);
        catch
          IFailure => Console.WriteLine($"case {i}: taken by the marker arm"),
        end try;
      catch
        var ex: Exception => Console.WriteLine($"case {i}: passed the marker arm: {ex.ToString()}"),
      end try;
    end for;
  end Main;
end Program;
