//! case: s27_5_8405
//! doc: spec
//! section: 27.5
//! title: two lambdas writing a captured by-value type-parameter parameter leave the last write
//!   visible
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 14.1
//! expect: run
//! stdout: 1290
//! notes: Pins #9060 (control on the string substitution). `w1` and `w2` each write `p`; the
//!   frame then returns the final `p` = q2 = qq2-{j}: length 4+d; sum = 800 + 490 = 1290.
module Conf;
import System;
public class Program
  public static method M<T>(p: T, q1: T, q2: T): T
    var w1 := lambda(): int => do p := q1; return 1; end;
    var w2 := lambda(): int => do p := q2; return 1; end;
    var a := w1() + w2() + w1() + w2();
    return p;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s := Program.M<string>($"fr-{j}", $"q1-{j}", $"qq2-{j}");
      t := t + s.Length;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
