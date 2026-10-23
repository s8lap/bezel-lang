//! case: s35_3_0035
//! doc: spec
//! section: 35.3
//! title: the same nested payload-free case written as a value is admitted
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call).
//! cites: 35.5
//! expect: run
//! output: -1
module Conf;

public class Outer
  public choice Inner
    A(x: int),
    Nothing;
  end Inner;
end Outer;

public class Program
  public static method Main()
    var o := Outer.Inner.Nothing;
    Console.WriteLine((case o of A(var x) => x, Nothing => -1));
  end Main;
end Program;
