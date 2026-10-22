//! case: s17_14_0018
//! doc: spec
//! section: 17.14
//! title: a uint plus an int constant whose sum overflows uint is a constant-expression compile
//!   error
//! rule: Constant evaluation fault in this case. "An evaluation **fault** — integer or `decimal` overflow, division or `mod` by zero, a
//!   shift count outside the §6.5 domain — is a **compile error at the constant
//!   expression**, never a deferred runtime fault" (§17.14, Evaluation, v0.340). The `int`
//!   constant narrows to `uint` (§5.1), and `4000000000 + 2000000000` exceeds `uint`'s
//!   maximum.
//! cites: 5.1, 6.1
//! expect: reject
//! diagnostic: overflow
module Conf;

public class Program
  public static method Main()
    const U: uint := 4000000000U;
    const V: int := 2000000000;
    Console.WriteLine(U + V);
  end Main;
end Program;
