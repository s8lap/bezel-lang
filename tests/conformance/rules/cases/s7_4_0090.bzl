//! case: s7_4_0090
//! doc: spec
//! section: 7.4
//! title: a non-null assignment in one branch and a null test in the other narrow a local after the join
//! rule: "assignment: after `x := expr;` where `expr`'s static type is non-nullable" ([§7.4]) and "`x = null` or `x is null` (narrowing the *false* branch)" ([§7.4]). "a fact holds at a point only when it holds on every path that reaches it" ([§7.4]): both paths to the read hold a non-null value.
//! expect: run
//! output: 2020
//!   2024
module Conf;
public class P
  static method Asg(n: DateTime?): int
    var m: DateTime? := n;
    if m = null then m := new DateTime(2020, 1, 2); end;
    return m.Year;
  end Asg;
  public static method Main(): int
    Console.WriteLine(Asg(null));
    Console.WriteLine(Asg(new DateTime(2024, 3, 15)));
    return 0;
  end Main;
end P;
