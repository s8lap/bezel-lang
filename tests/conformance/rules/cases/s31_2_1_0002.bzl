//! case: s31_2_1_0002
//! doc: spec
//! section: 31.2.1
//! title: a false guard skips the arm and falls through to the next
//! rule: Each pattern may be followed by an optional `when Expression` guard.
//!   The guard is evaluated only if the pattern matches; if the guard returns
//!   `false`, the arm is skipped and the next arm is considered.
//! cites: 31.2.1
//! expect: run
//! stdout: nonpositive
module Conf;

public class AmountException: Exception
  private _amount: int;

  public property Amount: int => _amount;

  public constructor(message: string, amount: int) base(message)
    _amount := amount;
  end constructor;
end AmountException;

public class Program
  public static method Main()
    try
      throw new AmountException("bad", -5);
    catch
      var ex: AmountException when ex.Amount > 0 => Console.WriteLine("positive"),
      var ex: AmountException => Console.WriteLine("nonpositive"),
    end try;
  end Main;
end Program;
