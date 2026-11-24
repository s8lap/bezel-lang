//! case: s27_5_8201
//! doc: spec
//! section: 27.5
//! title: an escaping writer closure with two compound appends to a string parameter reads its
//!   writes
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 3380
//! notes: Pins #9025/#9026 (cell of a written, shared captured string parameter). Two closures
//!   capture parameter `p` of `M`, so `p` lives in a shared cell; the writer appends twice
//!   per call and is returned, so it outlives `M`'s frame. Each call to the writer sees the
//!   previous call's value (the cell persists) and each appended value is the previous
//!   length. The arguments are fresh interpolated strings. Expected output is hand-computed
//!   from the program: 200 iterations with arguments `fr-<j>` (j = 0..199), sum of the string
//!   lengths the closures return = 3380. The corpus runner has no sanitizer leg, so this pins
//!   the stdout a use-after-free would destroy; it does not pin leaks.
module Conf;
public class Program
  public static method M(p: string): () -> string
    var f1 := lambda(): string => p;
    var f2 := lambda(): string => do p += "z"; p += $"{p.Length}"; return p; end;
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
