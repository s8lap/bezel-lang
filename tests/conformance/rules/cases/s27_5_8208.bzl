//! case: s27_5_8208
//! doc: spec
//! section: 27.5
//! title: an iterator method's string parameter written through a closure is read back correctly
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 4070
//! notes: Pins #9025/#9026 in an iterator body: the iterator's frame holds the parameter's cell,
//!   which the writer appends `i` to; the reader sees the appended value. Expected output is
//!   hand-computed from the program: 200 iterations with arguments `fr-<j>` (j = 0..199), sum
//!   of the string lengths the closures return = 4070. The corpus runner has no sanitizer
//!   leg, so this pins the stdout a use-after-free would destroy; it does not pin leaks.
module Conf;
public class Program
  public static method Gen(p: string): IEnumerable<int>
    var f1 := lambda(): string => p;
    var f2 := lambda(): string => do p := p + "i"; return p; end;
    yield return f2().Length;
    yield return f1().Length;
    yield return f2().Length;
  end Gen;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      for var n in Program.Gen($"fr-{j}") do t := t + n; end;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
