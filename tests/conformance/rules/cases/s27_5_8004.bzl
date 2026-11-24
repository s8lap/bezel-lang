//! case: s27_5_8004
//! doc: spec
//! section: 27.5
//! title: a nested lambda assigns a function local on either if branch and the read sees the
//!   taken one
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!   vice versa.
//! cites: 27.5
//! expect: run
//! stdout: 20106
//! notes: Pins the #3743 regression fix for a nested write under an if/else.
module Conf;
public class C
  public static method Run(n: int): int
    var outer := lambda(): int => do
      var f := lambda(): int => n;
      var mk := lambda(): int => do
        if n > 5 then f := lambda(): int => n + 1; else f := lambda(): int => n + 2; end;
        return 0;
      end;
      var t := mk();
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
