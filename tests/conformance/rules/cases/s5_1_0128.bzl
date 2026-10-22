//! case: s5_1_0128
//! doc: spec
//! section: 5.1
//! title: an int constant operand narrows to uint beside a uint variable in arithmetic and
//!   comparison
//! rule: Constant narrowing at a ulong site. "An expression of integer type that is a **compile-time constant** implicitly converts
//!   to any integer type whose range contains its value" (§5.1, Constant narrowing, v0.340).
//!   The mixed-operand promotion of §6.2 uses §5.1, so `const K: int := 7` is admissible
//!   beside a `uint` operand and the operation is typed `uint`.
//! cites: 6.2, 17.14
//! expect: run
//! output: True
//!   True
//!   3999999993
//!   False
//!   True
module Conf;

public class Program
  public static method Main()
    var u: uint := 4000000000U;
    const K: int := 7;
    Console.WriteLine(u > K);
    Console.WriteLine(K < u);
    Console.WriteLine(u - K);
    Console.WriteLine(u = K);
    Console.WriteLine(u <> K);
  end Main;
end Program;
