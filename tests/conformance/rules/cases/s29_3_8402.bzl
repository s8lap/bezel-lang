//! case: s29_3_8402
//! doc: spec
//! section: 29.3
//! title: an iterator method generic over T, called with a capturing closure, yields each
//!   element once
//! rule: the body runs incrementally as the consumer calls `MoveNext()`
//! cites: 29.3, 14.1, 27.5
//! expect: run
//! stdout: 400
//! notes: Pins #9071 with a capture-bearing function-typed argument (the local `k`). Same count
//!   as s29_3_8401: 400.
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
      for var n in Program.M<() -> int>(lambda(): int => k + 1) do t := t + n; end for;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
