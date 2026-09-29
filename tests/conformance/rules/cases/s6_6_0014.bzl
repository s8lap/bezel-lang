//! case: s6_6_0014
//! doc: spec
//! section: 6.6
//! title: a nullable reference-typed target of += is rejected
//! rule: A read-modify-write compound assignment is a compile error when the
//!   static type of its left-hand side is a nullable type — a nullable
//!   reference-typed target included: the desugared x op e has no meaning on a
//!   nullable operand — static type read after §7.4's narrowing.
//! cites: 6.6, 7.11
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Maybe(): string? => "a";

  public static method Main()
    var label: string? := Maybe();
    label += "b";
  end Main;
end Program;
