//! case: s27_5_8205
//! doc: spec
//! section: 27.5
//! title: a string parameter first captured under a condition is shared by the later escaping
//!   writer
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 2980
//! notes: Pins #9025/#9026 (conditional first capture): for even `k` a first writer runs inside
//!   `M` and appends `c`; the returned writer appends `d`. The cell is shared and persists
//!   across both. Expected output is hand-computed from the program: 200 iterations with
//!   arguments `fr-<j>` (j = 0..199), sum of the string lengths the closures return = 2980.
//!   The corpus runner has no sanitizer leg, so this pins the stdout a use-after-free would
//!   destroy; it does not pin leaks.
module Conf;
public class Program
  public static method M(p: string, k: int): () -> string
    var n := 0;
    if k mod 2 = 0 then
      var f1 := lambda(): string => do p := p + "c"; return p; end;
      n := f1().Length;
    end;
    var f2 := lambda(): string => do p := p + "d"; return p; end;
    return f2;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.M($"fr-{j}", j); t := t + f().Length + f().Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
