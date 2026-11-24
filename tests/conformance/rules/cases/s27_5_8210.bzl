//! case: s27_5_8210
//! doc: spec
//! section: 27.5
//! title: an async lambda's inner writer on a captured string parameter is read back across an
//!   await
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 5760
//! notes: Pins #9025/#9026 (nested capture under an async lambda): the writer closure runs before
//!   and after an await; the enclosing method's parameter `p` is read after the async lambda
//!   completes. Expected output is hand-computed from the program: 200 iterations with
//!   arguments `fr-<j>` (j = 0..199), sum of the string lengths the closures return = 5760.
//!   The corpus runner has no sanitizer leg, so this pins the stdout a use-after-free would
//!   destroy; it does not pin leaks.
module Conf;
public class Program
  public static method M(p: string): int
    var g := async lambda(): Task<int> => do
      var r1 := lambda(): string => p;
      var w := lambda(): string => do p := p + "z"; return p; end;
      var x := w();
      await Task.Delay(0);
      var y := w();
      return x.Length + y.Length + r1().Length;
    end;
    using var k := g();
    k.Wait();
    return k.Result + p.Length;
  end M;
  public static method Main(): int
    var t := 0;
    for var j in 0..<200 do
      t := t + Program.M($"fr-{j}");
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
