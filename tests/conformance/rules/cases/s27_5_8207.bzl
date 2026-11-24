//! case: s27_5_8207
//! doc: spec
//! section: 27.5
//! title: an enclosing lambda's captured string parameter written by an inner closure persists
//!   across calls
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 9740
//! notes: Pins #9025/#9026 (nested capture): the outer lambda captures `p` and its body forms an
//!   inner writer; both outer calls share the cell, so the second call starts from the first
//!   call's final value. Expected output is hand-computed from the program: 200 iterations
//!   with arguments `fr-<j>` (j = 0..199), sum of the string lengths the closures return =
//!   9740. The corpus runner has no sanitizer leg, so this pins the stdout a use-after-free
//!   would destroy; it does not pin leaks.
module Conf;
public class Program
  public static method M(p: string): () -> string
    return lambda(): string => do
      var r1 := lambda(): string => p;
      var w := lambda(): string => do p := p + "z"; return p; end;
      var a := w();
      var b := w();
      return a + b + r1();
    end;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var h := Program.M($"fr-{j}"); t := t + h().Length + h().Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
