//! case: s6_2_0004
//! doc: spec
//! section: 6.2
//! title: a constant operand converts to the other operand's uint type in either position, then
//!   widens to long
//! rule: A uint operand fixes the constant's type. "An expression of integer type that is a compile-time constant implicitly converts to
//!   any integer type whose range contains its value" (§5.1); "The result has the larger
//!   type." (§6.2, v0.340). With `u: uint`, `1 + u` and `u + 1` are uint (the int constant
//!   fits uint), and the uint result widens to a `long` parameter without wrapping.
//! cites: 5.1, 22.6
//! expect: run
//! output: 4000000001
//!   4000000001
//!   uint
//!   uint
module Conf;

public class Program
  public static method F(x: long): long => x;
  public static method H(x: uint): string => "uint";
  public static method H(x: long): string => "long";

  public static method Main()
    var u: uint := 4000000000U;
    Console.WriteLine(F(1 + u));
    Console.WriteLine(F(u + 1));
    Console.WriteLine(H(1 + u));
    Console.WriteLine(H(u + 1));
  end Main;
end Program;
