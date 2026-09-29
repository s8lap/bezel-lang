//! case: s6_6_0013
//! doc: spec
//! section: 6.6
//! title: a nullable right operand to a read-modify-write compound assignment is rejected
//! rule: A read-modify-write compound assignment is a compile error when the
//!   static type of its left-hand side, or of its right operand, is a nullable
//!   type — either position, or both — static type read after §7.4's
//!   narrowing.
//! cites: 6.6, 7.11
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Main()
    var total: int := 1;
    var delta: int? := int.ParseOrNull("2");
    total += delta;
    Console.WriteLine(total);
  end Main;
end Program;
