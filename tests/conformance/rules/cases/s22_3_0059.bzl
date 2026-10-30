//! case: s22_3_0059
//! doc: spec
//! section: 22.3
//! title: an owned parameter is relayed onward with the move marker
//! rule: the caller writes the transfer marker at the call site: `M(move x)`
//! cites: 22.3, 28.8
//! expect: run
//! output: 400
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
  public static method Relay(owned s: StringBuilder): int => Take(move s);
  public static method RelayA(owned a: Array<int>): int => TakeA(move a);
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do t += Relay(new StringBuilder("ab")) + RelayA([1, 2]); end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
