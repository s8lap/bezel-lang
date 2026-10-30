//! case: l4_3_0007
//! doc: stdlib
//! section: 4.3
//! title: fresh values and a moved local are stored into OwningArray slots
//! rule: public method Add(owned item: T);
//! cites: 4.5, 4.3, 28.3.4
//! expect: run
//! output: 800
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Fresh(): owned StringBuilder => new StringBuilder("ab");
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var o := new OwningArray<StringBuilder>();
      o.Add(new StringBuilder("ab"));
      o.Add(Fresh());
      var l := new StringBuilder("q");
      o.Add(move l);
      o[0] := new StringBuilder("zzz");
      o[1] := Fresh();
      t += o[0].Length + o[1].Length + o.Count;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
