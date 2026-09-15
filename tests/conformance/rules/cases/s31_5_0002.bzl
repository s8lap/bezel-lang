//! case: s31_5_0002
//! doc: spec
//! section: 31.5
//! title: throw with an operand may appear as an if-expression branch
//! rule: `throw` — with an operand, or the bare re-raise — may appear in any
//!   expression position; the bottom type unifies with the other branch's
//!   type in an if-expression.
//! cites: 31.5, 17.12
//! expect: run
//! stdout: not ok
module Conf;

public class Program
  public static method Pick(ok: bool): int =>
    (if ok then 1 else throw new InvalidOperationException("not ok"));

  public static method Main()
    try
      var v := Pick(false);
      Console.WriteLine(v);
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
