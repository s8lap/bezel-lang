//! case: s27_5_8401
//! doc: spec
//! section: 27.5
//! title: two lambdas writing a captured function-typed parameter are visible to the lambda
//!   that reads it
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.14
//! expect: run
//! stdout: 3470
//! notes: Pins #9039. The captured parameter `p` is written by `w1` and `w2` and read by `r`.
//!   Per j (bb=b{j}, cc=cc{j}, dd=ddd{j}): w1()=1, r()=len(cc)=2+d; w2()=1,
//!   r()=len(dd)=3+d; w1()=1, r()=2+d. Per j: 3 + (2+d) + (3+d) + (2+d)... summed over j =
//!   3470.
module Conf;
import System;
public class Program
  public static method M(p: () -> string, q1: () -> string, q2: () -> string): int
    var r := lambda(): int => p().Length;
    var w1 := lambda(): int => do p := q1; return 1; end;
    var w2 := lambda(): int => do p := q2; return 1; end;
    return w1() + r() + w2() + r() + w1() + r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var bb := $"b{j}";
      var cc := $"cc{j}";
      var dd := $"ddd{j}";
      t := t + Program.M(lambda(): string => bb, lambda(): string => cc, lambda(): string => dd);
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
