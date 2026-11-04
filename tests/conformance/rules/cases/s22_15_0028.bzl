//! case: s22_15_0028
//! doc: spec
//! section: 22.15
//! title: a built-in exception thrown by the throw expression is admitted in a pure body
//! rule: Every built-in exception type's `()` and `(message)`-form constructors
//!   (and `ArgumentException`/`ArgumentOutOfRangeException`'s `(message,
//!   paramName)` forms) are `pure`
//! cites: 22.15
//! expect: run
//! issue: 7453
//! output: ok
//!   3
//!   range
//!   arg
module Conf;

public class Ops
  public static pure method Req(s: string?): string => s ?? throw new ArgumentException("null s");
  public static pure method NonNeg(x: int): int => (if x >= 0 then x else throw new ArgumentOutOfRangeException("x", "neg"));
end Ops;

public class Program
  public static method Main()
    Console.WriteLine(Ops.Req("ok"));
    Console.WriteLine(Ops.NonNeg(3));
    try
      _ := Ops.NonNeg(-1);
    catch
      var e: ArgumentOutOfRangeException => Console.WriteLine("range"),
    end;
    try
      _ := Ops.Req(null);
    catch
      var e: ArgumentException => Console.WriteLine("arg"),
    end;
  end Main;
end Program;
