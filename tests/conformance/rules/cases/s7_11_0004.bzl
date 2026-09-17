//! case: s7_11_0004
//! doc: spec
//! section: 7.11
//! title: null-forgiving before a relational operator admits it
//! rule: The rule reads the operand's static type, so an operand asserted
//!   non-null with `!` is a non-nullable operand and the ordinary operator
//!   applies to it unchanged.
//! cites: 7.8, C.1
//! expect: run
//! stdout: less
module Conf;

public class Program
  public static method Main()
    var a: int? := 5;
    var b: int := 10;
    if a! < b then
      Console.WriteLine("less");
    end;
  end Main;
end Program;
