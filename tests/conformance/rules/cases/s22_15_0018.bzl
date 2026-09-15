//! case: s22_15_0018
//! doc: spec
//! section: 22.15
//! title: the throw carve-out admits a pure-constructed trivial-dispose exception operand
//! rule: Inside a pure body, `new E(args)` is admitted only as the
//!   immediate operand of a `throw` statement or `throw` expression, and
//!   only when the invoked constructor of E is pure-declared and E is a
//!   trivial-dispose class.
//! cites: 22.15
//! expect: run
//! stdout: caught
module Conf;

public class Ops
  public static pure method Checked(x: int, low: int, high: int): int
    if low > high then
      throw new ArgumentException("low must not exceed high", nameof(low));
    end;
    return (if x < low then low else (if x > high then high else x));
  end Checked;
end Ops;

public class Program
  public static method Main()
    try
      _ := Ops.Checked(5, 10, 1);
    catch
      var ex: ArgumentException => Console.WriteLine("caught"),
    end;
  end Main;
end Program;
