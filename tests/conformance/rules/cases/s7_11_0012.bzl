//! case: s7_11_0012
//! doc: spec
//! section: 7.11
//! title: unary minus and complement apply to a nullable int once a null test narrows it
//! rule: The rule reads the operand's static type, so an operand that §7.4 has
//!   narrowed non-null is a non-nullable operand and the ordinary operator
//!   applies to it unchanged; no separate rule is needed for that case.
//! cites: 7.11, 7.4
//! expect: run
//! output: -4
//!   -5
module Conf;

public class Program
  public static method Make(): int? => 4;

  public static method Main()
    var i: int? := Make();
    if i <> null then
      Console.WriteLine(-i);
      Console.WriteLine(~i);
    end;
  end Main;
end Program;
