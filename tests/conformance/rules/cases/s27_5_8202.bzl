//! case: s27_5_8202
//! doc: spec
//! section: 27.5
//! title: an escaping writer assigning a call result to a captured string parameter reads that
//!   value
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 1600
//! notes: Pins #9025/#9026: the written value is a call result (`mk-<length>`), not a literal. The
//!   second call sees the first call's result. Expected output is hand-computed from the
//!   program: 200 iterations with arguments `fr-<j>` (j = 0..199), sum of the string lengths
//!   the closures return = 1600. The corpus runner has no sanitizer leg, so this pins the
//!   stdout a use-after-free would destroy; it does not pin leaks.
module Conf;
public class Program
  public static method Mk(k: int): string
    return $"mk-{k}";
  end Mk;
  public static method M(p: string): () -> string
    var f1 := lambda(): string => p;
    var f2 := lambda(): string => do p := Program.Mk(p.Length); return p; end;
    var a := f1();
    return f2;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.M($"fr-{j}"); t := t + f().Length + f().Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
