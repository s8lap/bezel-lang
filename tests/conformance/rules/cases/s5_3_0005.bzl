//! case: s5_3_0005
//! doc: spec
//! section: 5.3
//! title: a nullable cast to an enum applies the declared-constant check to a present value
//! rule: A present value v converts by T(v) under that conversion's own
//!   semantics, including its runtime checks — for an enum target, §5.4's
//!   declared-constant check included.
//! cites: 5.3, 5.4
//! expect: run
//! stdout: overflow
module Conf;

public enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Main()
    try
      var n: int? := 5;
      var p := Priority?(n);
      Console.WriteLine($"[{p}]");
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
