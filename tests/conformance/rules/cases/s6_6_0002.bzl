//! case: s6_6_0002
//! doc: spec
//! section: 6.6
//! title: a compound assignment's narrow-integer write-back raises OverflowException out of range
//! rule: b += 10 on b: byte is b := byte(b + 10) — the addition runs in int
//!   per §6.2, and a result outside byte's range raises OverflowException —
//!   never a silent truncation.
//! cites: 6.6, 6.1
//! expect: run
//! stdout: overflow
module Conf;

public class Program
  public static method Main()
    try
      var b: byte := 250;
      b += 10;
      Console.WriteLine(b);
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
