//! case: s27_5_8209
//! doc: spec
//! section: 27.5
//! title: an async method's string parameter written through a closure across awaits is read back
//!   correctly
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 6160
//! notes: Pins #9025/#9026 in an async body: the writer runs once per loop iteration across `await
//!   Task.Delay(0)`, the reader runs after the loop. Expected output is hand-computed from
//!   the program: 200 iterations with arguments `fr-<j>` (j = 0..199), sum of the string
//!   lengths the closures return = 6160. The corpus runner has no sanitizer leg, so this pins
//!   the stdout a use-after-free would destroy; it does not pin leaks.
module Conf;
public class Program
  public static async method M(p: string): Task<int>
    var f1 := lambda(): string => p;
    var f2 := lambda(): string => do p := p + "z"; return p; end;
    var n := 0;
    for var i in 0..<3 do
      n := n + f2().Length;
      await Task.Delay(0);
    end;
    return n + f1().Length;
  end M;
  public static method Main(): int
    var t := 0;
    for var j in 0..<200 do
      using var k := Program.M($"fr-{j}");
      k.Wait();
      t := t + k.Result;
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
