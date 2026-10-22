//! case: s5_1_0131
//! doc: spec
//! section: 5.1
//! title: a negative int constant does not narrow to uint beside a uint variable in a comparison
//! rule: "A constant whose value does not fit the target's range is a **compile error** at the
//!   conversion" (§5.1, Constant narrowing, v0.340). `-1` is outside `uint`'s range, so the
//!   comparison `u > K` has no common operand type (§6.2).
//! cites: 6.2
//! expect: reject
//! diagnostic: does not fit
module Conf;

public class Program
  public static method Main()
    const K: int := -1;
    var u: uint := 250U;
    Console.WriteLine(u > K);
  end Main;
end Program;
