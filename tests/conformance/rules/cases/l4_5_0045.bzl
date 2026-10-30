//! case: l4_5_0045
//! doc: stdlib
//! section: 4.5
//! title: PopFirst and PopAt results pass unmarked to owned parameters
//! rule: public method PopAt(index: int): owned T;
//! cites: 4.5, 22.3
//! expect: run
//! output: 500
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
      o.Add(new StringBuilder("cde"));
      t += Take(o.PopFirst()) + Take(o.PopAt(0));
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
