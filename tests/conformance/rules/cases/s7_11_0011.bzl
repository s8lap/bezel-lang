//! case: s7_11_0011
//! doc: spec
//! section: 7.11
//! title: unary minus over an un-narrowed nullable int is rejected
//! rule: an operand whose static type is a nullable value type is a compile error
//!   at every arithmetic, bitwise, shift, and relational operator. The rejected
//!   operators. The binary +, -, *, /, mod, &, |, ^, <<, >>; the unary +, -, ~;
//!   and the relational <, <=, >, >=.
//! cites: 7.11
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Make(): int? => 4;

  public static method Main()
    var i: int? := Make();
    Console.WriteLine(-i);
  end Main;
end Program;
