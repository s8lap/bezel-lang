//! case: s5_1_0130
//! doc: spec
//! section: 5.1
//! title: a negative int constant does not narrow to uint beside a uint constant operand
//! rule: "A constant whose value does not fit the target's range is a **compile error** at the
//!   conversion" (§5.1, Constant narrowing, v0.340). `-7` is outside `uint`'s range, and
//!   `uint` and `int` have no non-constant implicit conversion either way (§5.1), so there
//!   is no common operand type (§6.2). The twin with `const V: int := 7` is the accepting
//!   case before this one.
//! cites: 6.2
//! expect: reject
//! diagnostic: does not fit
module Conf;

public class Program
  public static method Main()
    const U: uint := 4000000000U;
    const V: int := -7;
    Console.WriteLine(U + V);
  end Main;
end Program;
