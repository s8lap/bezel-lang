//! case: s28_3_0314
//! doc: spec
//! section: 28.3
//! title: an owned collection parameter's PopFirst is returned under an owned return
//! rule: Every return path supplies a fresh or owned value (`null` admissible under a nullable
//!   declared type
//! cites: 28.3.5, 22.3
//! expect: run
//! output: 200
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method First(owned o: OwningArray<StringBuilder>): owned StringBuilder
    using var k := move o;
    return k.PopFirst();
  end First;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      var o := new OwningArray<StringBuilder>();
      o.Add(new StringBuilder("ab"));
      t += First(move o).Length;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
