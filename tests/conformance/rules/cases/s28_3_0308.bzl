//! case: s28_3_0308
//! doc: spec
//! section: 28.3
//! title: an owned return supplied by new Array<int>() is accepted
//! rule: Every return path supplies a fresh or owned value (`null` admissible under a nullable
//!   declared type
//! cites: 28.3.5
//! expect: run
//! output: 100
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Mk(): owned Array<int> => new Array<int>();
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do t += Mk().Count + 1; end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
