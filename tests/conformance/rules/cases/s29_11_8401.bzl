//! case: s29_11_8401
//! doc: spec
//! section: 29.11
//! title: an iterator result generic over a function-typed T is enumerated twice identically
//! rule: Iterating the same source twice with two `for` loops works as expected for collection
//!   types — and for an iterator method's result
//! cites: 29.11, 29.3, 14.1
//! expect: run
//! output: 800
//! notes: Pins #9071's clone site (two enumerations of one `IEnumerable<int>`). A function type
//!   is not move-only, so the result is re-iterable: each loop yields 2, so 4 per call;
//!   200 calls = 800.
module Conf;
import System;
public class Program
  public static method M<T>(p: T): IEnumerable<int>
    var r := lambda(): int => do var y := p; return 1; end;
    yield return r();
    yield return r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var k := 3;
      var e := Program.M<() -> int>(lambda(): int => k + 1);
      for var n in e do t := t + n; end for;
      for var n in e do t := t + n; end for;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
