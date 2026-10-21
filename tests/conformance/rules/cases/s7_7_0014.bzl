//! case: s7_7_0014
//! doc: spec
//! section: 7.7
//! title: a negative constant is not implicitly convertible to the byte left type of ??
//! rule: "for a left operand of type `T?`, the right operand must be implicitly convertible" ([§5.1])
//!   "to `T` or to `T?`". "A constant whose value does not fit the target's range is a **compile error** at the conversion site" ([§5.1]), and `-1` does not fit `byte`.
//! cites: 7.7, 5.1
//! expect: reject
//! diagnostic: does not fit
//! issue: 6214
module Conf;

public class Program
  public static method Main()
    var b: byte? := null;
    Console.WriteLine(int(b ?? -1));
  end Main;
end Program;
