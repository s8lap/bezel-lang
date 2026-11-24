//! case: s27_5_8407
//! doc: spec
//! section: 27.5
//! title: a lambda writing a call-bounded function parameter through a captured ref parameter
//!   is rejected
//! rule: a function value may not be stored in a field, in any collection, in a property or
//!   indexer, or through an `out` or `ref` parameter
//! cites: 28.3, 28.14, 27.5
//! expect: reject
//! diagnostic: scoped
//! notes: Pins the boundary of #9039's ref sibling: `q` is a plain (call-bounded) function
//!   parameter, so its source set is non-empty, and `p := q` inside `w` stores it through
//!   the `ref` parameter `p`, which outlives the call. Twin: s27_5_8406 (the stored value
//!   is capture-free).
module Conf;
import System;
public class Program
  public static method M(ref p: () -> string, q: () -> string): int
    var r := lambda(): int => p().Length;
    var w := lambda(): int => do p := q; return 1; end;
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
