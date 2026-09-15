//! case: s5_3_0003
//! doc: spec
//! section: 5.3
//! title: a nullable cast of a present value that does not fit raises OverflowException
//! rule: A present value v converts by T(v) under that conversion's own
//!   semantics, including its runtime checks (OverflowException on a
//!   non-fitting present value).
//! cites: 5.3, 5.2
//! expect: run
//! stdout: overflow
module Conf;

public class Program
  public static method Main()
    try
      var x: int? := 300;
      var b := byte?(x);
      Console.WriteLine($"[{b}]");
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
