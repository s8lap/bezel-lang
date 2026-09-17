//! case: s7_11_0002
//! doc: spec
//! section: 7.11
//! title: coalescing a nullable operand to a value admits the arithmetic operator
//! rule: No operator lifts to a nullable type; the remedy is `??`, `!`, or a
//!   narrowing check before the operator applies.
//! cites: 7.7, C.1
//! expect: run
//! stdout: 7
module Conf;

public class Program
  public static method Main()
    var a: int? := 5;
    var b: int := 2;
    var c := (a ?? 0) + b;
    Console.WriteLine(c);
  end Main;
end Program;
