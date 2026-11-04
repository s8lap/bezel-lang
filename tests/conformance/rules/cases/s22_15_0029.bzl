//! case: s22_15_0029
//! doc: spec
//! section: 22.15
//! title: a throw expression in a case arm is admitted in a pure body
//! rule: Inside a pure body, `new E(args)` is admitted only as the immediate
//!   operand of a `throw` statement or `throw` expression, and only when
//! cites: 22.15
//! expect: run
//! issue: 7453
//! output: zero
//!   one
//!   range
module Conf;

public class Ops
  public static pure method Name(x: int): string => (case x of 0 => "zero", 1 => "one", _ => throw new ArgumentOutOfRangeException("x"));
end Ops;

public class Program
  public static method Main()
    Console.WriteLine(Ops.Name(0));
    Console.WriteLine(Ops.Name(1));
    try
      _ := Ops.Name(2);
    catch
      var e: ArgumentOutOfRangeException => Console.WriteLine("range"),
    end;
  end Main;
end Program;
