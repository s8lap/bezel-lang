//! case: s27_5_8206
//! doc: spec
//! section: 27.5
//! title: a lambda's own string parameter captured by two inner closures is shared with the
//!   escaping writer
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 2780
//! notes: Pins #9025/#9026 for a lambda's own parameter (rather than a method's): the writer
//!   returned from the lambda appends `z` to `q`, whose cell outlives the call. Expected
//!   output is hand-computed from the program: 200 iterations with arguments `fr-<j>` (j =
//!   0..199), sum of the string lengths the closures return = 2780. The corpus runner has no
//!   sanitizer leg, so this pins the stdout a use-after-free would destroy; it does not pin
//!   leaks.
module Conf;
public class Program
  public static method M(): (string) -> () -> string
    return lambda(q: string): () -> string => do
      var r1 := lambda(): string => q;
      var w := lambda(): string => do q := q + "z"; return q; end;
      var a := r1();
      return w;
    end;
  end M;
  public static method Run(): int
    var t := 0;
    var g := Program.M();
    for var j in 0..<200 do
      var h := g($"fr-{j}"); t := t + h().Length + h().Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
