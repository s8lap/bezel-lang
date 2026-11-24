//! case: s27_5_8211
//! doc: spec
//! section: 27.5
//! title: an escaping reader of a generic method's captured parameter at T = string reads its
//!   argument
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 1090
//! notes: Pins #9025 for a generic method instantiated at `string`: `p` is captured by a writer
//!   that never runs and by the returned reader; the reader must still see the caller's
//!   argument after the call returned. Reads (never-run writer): the result is the sum of the
//!   argument lengths. Expected output is hand-computed from the program: 200 iterations with
//!   arguments `fr-<j>` (j = 0..199), sum of the string lengths the closures return = 1090.
//!   The corpus runner has no sanitizer leg, so this pins the stdout a use-after-free would
//!   destroy; it does not pin leaks.
module Conf;
public class Program
  public static method G<T>(p: T, q: T): () -> T
    var w := lambda() => do p := q; end;
    var r := lambda(): T => p;
    return r;
  end G;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.G<string>($"fr-{j}", $"q-{j}"); t := t + f().Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
