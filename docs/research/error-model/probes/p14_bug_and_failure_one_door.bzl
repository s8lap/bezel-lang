// Probe p14: today a bug and an expected failure leave a call through the same
// door. A contract violation, an index fault and an input failure are all
// catchable exceptions, and one `Exception` arm takes all three (spec §31.4,
// §22.16). Nothing at the call or in the signature separates the two bands.
module P14;

public class Account
  private _balance: int;
  public constructor(start: int)
    _balance := start;
  end constructor;
  public method Withdraw(amount: int)
    requires(amount > 0)
    _balance := _balance - amount;
  end Withdraw;
end Account;

public class Program
  public static method Run(which: int)
    using var acct := new Account(10);
    using var xs := new Array<int>(2, 0);
    if which = 0 then
      acct.Withdraw(-5);                 // caller bug: contract violation
    elif which = 1 then
      Console.WriteLine(xs[9]);          // bug: index out of range
    else
      Console.WriteLine(int.Parse("x")); // expected failure: bad input
    end if;
  end Run;

  public static method Main()
    for var i in 0..2 do
      try
        Program.Run(i);
      catch
        var ex: Exception => Console.WriteLine($"case {i}: one arm caught {ex.ToString()}"),
      end try;
    end for;
  end Main;
end Program;
