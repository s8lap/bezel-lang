//! case: s17_6_0013
//! doc: spec
//! section: 17.6
//! title: ++ past a narrow integer target's range raises OverflowException
//! rule: "`x++` is equivalent to `x := x + 1` ... under exactly §6.6's rules:
//!   the same overflow semantics as `+` and `-` (§6.1), §6.6's checked
//!   narrowing for a narrow integer target."
//! cites: 17.6, 6.6, 6.1
//! expect: run
//! stdout: overflow
module Conf;

public class Program
  public static method Main()
    try
      var b: byte := 255;
      b++;
      Console.WriteLine(b);
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
