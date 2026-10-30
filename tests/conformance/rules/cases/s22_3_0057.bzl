//! case: s22_3_0057
//! doc: spec
//! section: 22.3
//! title: fresh producers pass unmarked to owned parameters
//! rule: stays unmarked: no live binding is invalidated, so there is nothing to announce
//! cites: 22.3, 28.3.4
//! expect: run
//! output: 1500
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Take(owned s: StringBuilder): int
    return s.Length;
  end Take;
  public static method TakeA(owned a: Array<int>): int
    return a.Count;
  end TakeA;
  public static method Fresh(): owned StringBuilder => new StringBuilder("ab");
  public static method Main(): int
    var xs: Array<int> := [1, 2, 3];
    var t := 0;
    for var i in 0..<100 do
      t += Take(new StringBuilder("ab")) + Take(Fresh());
      t += TakeA([1, 2]) + TakeA(xs.ToArray()) + TakeA(xs[0..<1]) + TakeA(xs.Slice(1..<3)) + TakeA(xs.Sorted());
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
