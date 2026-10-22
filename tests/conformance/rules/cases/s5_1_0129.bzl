//! case: s5_1_0129
//! doc: spec
//! section: 5.1
//! title: two integer constants of types uint and int fold with the int constant narrowed to
//!   uint
//! rule: Constant narrowing in a comparison. "An expression of integer type that is a **compile-time constant** implicitly converts
//!   to any integer type whose range contains its value" (§5.1, Constant narrowing, v0.340).
//!   Both operands are constants, so the narrowing applies and the expression is typed
//!   `uint`; a `uint` sum is exact (§6.1).
//! cites: 6.2, 17.14
//! expect: run
//! output: 4000000007
//!   True
//!   True
//!   3999999993
module Conf;

public class Program
  public static method Main()
    const U: uint := 4000000000U;
    const V: int := 7;
    Console.WriteLine(U + V);
    Console.WriteLine(U > V);
    Console.WriteLine(V < U);
    Console.WriteLine(U - V);
  end Main;
end Program;
