//! case: s7_7_0021
//! doc: spec
//! section: 7.7
//! title: ??= over a non-nullable int target with a string right operand is rejected
//! rule: "Over a non-nullable target, §7.7's non-nullable-left rule applies: `e` must still convert to the target's type (`i ??= "s"` over `i: int` is a compile error)" (§6.6);
//!   "A non-nullable left operand of type `T` types exactly as a left operand of type `T?` would: the right operand must still be implicitly convertible to `T` or to `T?`" (§7.7). Accepting twin: s7_7_0026.
//! cites: 7.7, 6.6, 5.1
//! expect: reject
//! diagnostic: convert
//! issue: 8596
module Conf;

public class Program
  public static method Main()
    var i: int := 1;
    i ??= "s";
    Console.WriteLine(i);
  end Main;
end Program;
