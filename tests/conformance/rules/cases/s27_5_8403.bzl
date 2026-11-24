//! case: s27_5_8403
//! doc: spec
//! section: 27.5
//! title: a lambda's own function-typed parameter written by a nested lambda is read by another
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.14
//! expect: run
//! stdout: 890
//! notes: Pins #9039 (a lambda's own parameter as the captured cell). `w` writes `p := q`; `r`
//!   reads `p`. Per j: w()=1, r()=len(c{j})=1+d: sum = 200*2 + 490 = 890.
module Conf;
import System;
public class Program
  public static method Run(): int
    var t := 0;
    var g := lambda(p: () -> string, q: () -> string): int => do
      var r := lambda(): int => p().Length;
      var w := lambda(): int => do p := q; return 1; end;
      return w() + r();
    end;
    for var j in 0..<200 do
      var bb := $"b{j}";
      var cc := $"c{j}";
      t := t + g(lambda(): string => bb, lambda(): string => cc);
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
