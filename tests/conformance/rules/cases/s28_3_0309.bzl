//! case: s28_3_0309
//! doc: spec
//! section: 28.3
//! title: a collection literal returned under an owned return is accepted
//! rule: Collection literal (`[1, 2, 3]`) targeting a reference type
//! cites: 28.3.4, 15.4
//! expect: run
//! output: 300
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Mk(): owned Array<int>
    return [1, 2, 3];
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do t += Mk().Count; end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
