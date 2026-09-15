//! case: s7_11_0001
//! doc: spec
//! section: 7.11
//! title: arithmetic + does not lift over a nullable operand
//! rule: No operator lifts to a nullable type. An operand whose static type
//!   is a nullable value type is a compile error at every arithmetic
//!   operator.
//! cites: C.1
//! expect: reject
//! diagnostic: lifts
module Conf;

public class Program
  public static method Main()
    var a: int? := 5;
    var b: int := 2;
    var c := a + b;
    Console.WriteLine(c);
  end Main;
end Program;
