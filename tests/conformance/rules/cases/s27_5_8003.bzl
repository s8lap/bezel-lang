//! case: s27_5_8003
//! doc: spec
//! section: 27.5
//! title: a function local assigned by a nested lambda then by its own body reads the last
//!   write
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!   vice versa.
//! cites: 27.5
//! expect: run
//! stdout: 21300
//! notes: Pins the #3743 regression fix (both the nested write and the body write act on one
//!   shared binding).
module Conf;
public class C
  public static method Run(n: int): int
    var outer := lambda(): int => do
      var f := lambda(): int => n;
      var mk := lambda(): int => do f := lambda(): int => n + 1; return 0; end;
      var t := mk();
      f := lambda(): int => n + 7;
      return t + f();
    end;
    return outer();
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + Run(j); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
