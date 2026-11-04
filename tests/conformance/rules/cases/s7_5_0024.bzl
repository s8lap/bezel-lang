//! case: s7_5_0024
//! doc: spec
//! section: 7.5
//! title: a plain suffix after ?. continues the chain over a non-null and a null string?
//! rule: The chain is the whole postfix sequence (A.7's `PrimaryExpr`), its
//!   plain `.`, `[ ]` and `( )` suffixes included, and a suffix to the right of
//!   a `?.` is typed at the value it meets on the path where it runs — the
//!   preceding segment's own declared type, never the nullable type the `?.`
//!   gives the chain's result: `s?.Length.ToString()` over `s: string?` calls
//!   `ToString()` on an `int`, and the chain is a `string?`.
//! cites: 7.5, 7.4, 18.5
//! expect: run
//! output: 3
//!   null
//! issue: 6070
module Conf;

public class Program
  public static method Pick(present: bool): string?
    if present then
      return "abc";
    end;
    return null;
  end Pick;

  public static method Main()
    var a := Pick(true);
    var b := Pick(false);
    var ra: string? := a?.Length.ToString();
    var rb: string? := b?.Length.ToString();
    Console.WriteLine($"{ra}");
    Console.WriteLine($"{rb}");
  end Main;
end Program;
