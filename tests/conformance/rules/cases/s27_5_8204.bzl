//! case: s27_5_8204
//! doc: spec
//! section: 27.5
//! title: an escaping writer copying a captured string parameter before overwriting it keeps the
//!   old copy
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 5160
//! notes: Pins #9025/#9026: the closure copies the captured string to a local `q`, then writes
//!   `p`; the copy keeps the pre-write value, so the result has length (old + new). Expected
//!   output is hand-computed from the program: 200 iterations with arguments `fr-<j>` (j =
//!   0..199), sum of the string lengths the closures return = 5160. The corpus runner has no
//!   sanitizer leg, so this pins the stdout a use-after-free would destroy; it does not pin
//!   leaks.
module Conf;
public class Program
  public static method M(p: string): () -> string
    var f1 := lambda(): string => p;
    var f2 := lambda(): string => do var q := p; p := p + "z"; return q + p; end;
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
