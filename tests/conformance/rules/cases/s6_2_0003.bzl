//! case: s6_2_0003
//! doc: spec
//! section: 6.2
//! title: a binary operator over literal operands yields the larger operand type, which selects
//!   the overload
//! rule: Literal operands select an overload. "An expression of integer type that is a compile-time constant implicitly converts to
//!   any integer type whose range contains its value" (§5.1); "The result has the larger
//!   type." (§6.2, v0.340). `1 + 2` is int, `1 + 2L` long, `1U + 2` uint (the constant 2
//!   converts to uint), `1UL + 2` ulong and `1 + 2.0` double.
//! cites: 5.1
//! expect: run
//! output: int
//!   long
//!   uint
//!   ulong
//!   double
module Conf;

public class Program
  public static method S(x: int): string => "int";
  public static method S(x: long): string => "long";
  public static method S(x: uint): string => "uint";
  public static method S(x: ulong): string => "ulong";
  public static method S(x: double): string => "double";

  public static method Main()
    Console.WriteLine(S(1 + 2));
    Console.WriteLine(S(1 + 2L));
    Console.WriteLine(S(1U + 2));
    Console.WriteLine(S(1UL + 2));
    Console.WriteLine(S(1 + 2.0));
  end Main;
end Program;
