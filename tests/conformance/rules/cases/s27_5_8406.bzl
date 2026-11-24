//! case: s27_5_8406
//! doc: spec
//! section: 27.5
//! title: a lambda writing a capture-free function value through a captured ref parameter is
//!   visible
//! rule: a `ref` parameter aliases the caller's location, so a write through the closure is a
//!   write to it
//! cites: 27.5, 22.3
//! expect: run
//! stdout: 1400
//! notes: Pins #9039's ref sibling and the admitting twin of s27_5_8407. The write stores a
//!   capture-free lambda `"zzz"` through `ref p`; `w()` = 1, `r()` = 3, so M = 4, and the
//!   caller then reads `f()` = "zzz" = 3. Per j 7; 200*7 = 1400.
module Conf;
import System;
public class Program
  public static method M(ref p: () -> string, q: () -> string): int
    var r := lambda(): int => p().Length;
    var w := lambda(): int => do p := lambda(): string => "zzz"; return 1; end;
    var a := w();
    return a + r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var bb := $"b{j}";
      var cc := $"c{j}";
      var f := lambda(): string => bb;
      t := t + Program.M(ref f, lambda(): string => cc);
      t := t + f().Length;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
