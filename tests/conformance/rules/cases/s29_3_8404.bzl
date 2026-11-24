//! case: s29_3_8404
//! doc: spec
//! section: 29.3
//! title: an iterator method generic over T that yields its function-typed parameter yields it
//!   each time
//! rule: the body runs incrementally as the consumer calls `MoveNext()`
//! cites: 29.3, 14.1
//! expect: run
//! stdout: 400
//! notes: Pins #9071 (the parameter is the yielded element). Two yields of `p` per call, each
//!   bound to a local; 200 calls = 400.
module Conf;
import System;
public class Program
  public static method S<T>(p: T): IEnumerable<T>
    yield return p;
    yield return p;
  end S;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var k := 3;
      for var q in Program.S<() -> int>(lambda(): int => k + 1) do
        var z := q;
        t := t + 1;
      end for;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
