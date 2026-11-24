//! case: s29_3_8403
//! doc: spec
//! section: 29.3
//! title: two nested iterations over iterator results generic over a function-typed T each
//!   yield every element
//! rule: the body runs incrementally as the consumer calls `MoveNext()`
//! cites: 29.3, 14.1, 27.5
//! expect: run
//! stdout: 1600
//! notes: Pins #9071 (two frames alive at once). Outer n over 2 yields, inner m over 2 yields,
//!   `t += n + m` = 2 per pair: 4 pairs = 8 per call; 200 calls = 1600.
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
      for var n in Program.M<() -> int>(lambda(): int => k + 1) do
        for var m in Program.M<() -> int>(lambda(): int => k + 1) do t := t + n + m; end for;
      end for;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
