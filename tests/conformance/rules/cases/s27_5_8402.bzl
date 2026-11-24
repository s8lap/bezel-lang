//! case: s27_5_8402
//! doc: spec
//! section: 27.5
//! title: two lambdas writing a captured function-typed parameter inside a loop leave the last
//!   write visible
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.14
//! expect: run
//! stdout: 1490
//! notes: Pins #9039. `w1()` and `w2()` run once each in the loop body, then `r()` reads `p`,
//!   which the last write set to `dd`: per j 2 + len(ddd{j}) = 5 + d; sum = 1000 + 490 =
//!   1490.
module Conf;
import System;
public class Program
  public static method M(p: () -> string, q1: () -> string, q2: () -> string, n: int): int
    var r := lambda(): int => p().Length;
    var w1 := lambda(): int => do p := q1; return 1; end;
    var w2 := lambda(): int => do p := q2; return 1; end;
    var t := 0;
    for var i in 0..<n do
      t := t + w1() + w2();
    end for;
    return t + r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var bb := $"b{j}";
      var cc := $"cc{j}";
      var dd := $"ddd{j}";
      t := t + Program.M(lambda(): string => bb, lambda(): string => cc, lambda(): string => dd, 1);
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
