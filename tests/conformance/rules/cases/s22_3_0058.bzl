//! case: s22_3_0058
//! doc: spec
//! section: 22.3
//! title: named owned locals pass to owned parameters with the move marker
//! rule: the caller writes the transfer marker at the call site: `M(move x)`
//! cites: 22.3, 28.3.4
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
      var s := new StringBuilder("ab");
      var a := new Array<int>();
      a.Add(1);
      t += Take(move s) + TakeA(move a);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
