//! case: s29_3_8401
//! doc: spec
//! section: 29.3
//! title: an iterator method generic over T, called at a capture-free function type, yields
//!   each element once
//! rule: the body runs incrementally as the consumer calls `MoveNext()`
//! cites: 29.3, 14.1, 27.5
//! expect: run
//! stdout: 400
//! notes: Pins #9071: the iterator frame holds its own reference on the function-typed `p` at
//!   this instantiation. The body yields r() = 1 twice; the consumer sums 2 per call; 200
//!   calls = 400.
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
      for var n in Program.M<() -> int>(lambda(): int => 1) do t := t + n; end for;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
