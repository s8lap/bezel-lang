//! case: s27_5_8203
//! doc: spec
//! section: 27.5
//! title: an escaping writer that conditionally appends to a captured string parameter reads its
//!   writes
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 3570
//! notes: Pins #9025/#9026: the write sits in a branch whose condition depends on the current
//!   length parity and the parameter `k`, so some calls append and some do not; the shared
//!   cell persists across calls. Expected output is hand-computed from the program: 200
//!   iterations with arguments `fr-<j>` (j = 0..199), sum of the string lengths the closures
//!   return = 3570. The corpus runner has no sanitizer leg, so this pins the stdout a use-
//!   after-free would destroy; it does not pin leaks.
module Conf;
public class Program
  public static method M(p: string, k: int): () -> string
    var f1 := lambda(): string => p;
    var f2 := lambda(): string => do if p.Length mod 2 = k mod 2 then p := p + "b"; end; return p; end;
    var a := f1();
    return f2;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.M($"fr-{j}", j); t := t + f().Length + f().Length + f().Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
