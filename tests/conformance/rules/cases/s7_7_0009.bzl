//! case: s7_7_0009
//! doc: spec
//! section: 7.7
//! title: a non-nullable int right operand of ?? into a double? left operand has type double
//! rule: "the result type is `T` when the right operand is non-nullable" ([§7.7]), and the right operand is an `int` converted to `double`:
//!   "Implicit conversions to `float` from integers wider than 24 bits, and to `double` from integers wider than 53 bits, may lose precision but never overflow" ([§5.1]).
//!   "`R` and the **no-format `ToString()`** both produce the shortest string that parses back to the identical value" ([stdlib §2.18]).
//! cites: 7.7, 5.1
//! expect: run
//! output: 3
//! issue: 6214
module Conf;

public class Program
  public static method Main()
    var a: double? := null;
    var r: double := a ?? 3;
    Console.WriteLine(r);
  end Main;
end Program;
