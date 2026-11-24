//! case: s27_5_8404
//! doc: spec
//! section: 27.5
//! title: a closure written through a generic function-typed parameter is visible to the
//!   reading lambda
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 14.1, 28.14
//! expect: run
//! stdout: 890
//! notes: Pins #9060/#9039 (the type-parameter instantiation at string). `w` writes `p := q`,
//!   then `r` reads `p`: len(q-{j}) = 2 + d; sum = 400 + 490 = 890.
module Conf;
import System;
public class Program
  public static method M<T>(p: () -> T, q: () -> T): T
    var r := lambda(): T => p();
    var w := lambda(): int => do p := q; return 1; end;
    var a := w();
    return r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var bb := $"b{j}";
      var cc := $"q-{j}";
      var s := Program.M<string>(lambda(): string => bb, lambda(): string => cc);
      t := t + s.Length;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
