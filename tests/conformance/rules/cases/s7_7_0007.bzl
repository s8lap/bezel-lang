//! case: s7_7_0007
//! doc: spec
//! section: 7.7
//! title: a long constant outside the int range is rejected as the right operand of ?? on an int?
//! rule: "for a left operand of type `T?`, the right operand must be implicitly convertible" ([§5.1])
//!   "to `T` or to `T?`; the result type is `T` when the right operand is non-nullable and `T?` when it is nullable".
//!   "A constant whose value does not fit the target's range is a **compile error** at the conversion site" ([§5.1]).
//! cites: 7.7, 5.1
//! expect: reject
//! diagnostic: does not fit
//! issue: 6214
module Conf;

public class Program
  public static method Main()
    var a: int? := null;
    Console.WriteLine(a ?? 5000000000L);
  end Main;
end Program;
