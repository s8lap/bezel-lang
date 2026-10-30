//! case: l4_3_0006
//! doc: stdlib
//! section: 4.3
//! title: a Drain loop variable passes to an owned parameter with the move marker
//! rule: For draining iteration that transfers ownership, the owning collections expose `Drain()`
//!   returning `IOwningEnumerable<T>`
//! cites: 4.3, 22.3
//! expect: run
//! output: 300
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
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var o := new OwningArray<StringBuilder>();
      o.Add(new StringBuilder("ab"));
      o.Add(new StringBuilder("c"));
      for var x in o.Drain() do
        t += Take(move x);
      end for;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
