//! case: s29_3_8406
//! doc: spec
//! section: 29.3
//! title: an iterator with two type parameters, one function-typed, yields each element once
//! rule: the body runs incrementally as the consumer calls `MoveNext()`
//! cites: 29.3, 14.1
//! expect: run
//! stdout: 1600
//! notes: Pins #9071 (mixed instantiations of one generic iterator: `<() -> int, () -> string>`
//!   and `<() -> int, int>`). Each loop yields 2 of k = 2: 4 per loop, 8 per call; 200
//!   calls = 1600.
module Conf;
import System;
public class Program
  public static method M<T, U>(p: T, q: U, k: int): IEnumerable<int>
    var r := lambda(): int => do var y := p; var z := q; return k; end;
    yield return r();
    yield return r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      for var n in Program.M<() -> int, () -> string>(lambda(): int => 5, lambda(): string => "z", 2) do t := t + n; end for;
      for var n in Program.M<() -> int, int>(lambda(): int => 5, 4, 2) do t := t + n; end for;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
