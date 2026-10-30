//! case: s28_3_0313
//! doc: spec
//! section: 28.3
//! title: a generic owned T parameter and return pass owned values through
//! rule: Every return path supplies a fresh or owned value (`null` admissible under a nullable
//!   declared type
//! cites: 28.3.5, 28.1
//! expect: run
//! output: 300
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Id<T>(owned x: T): owned T where T: class => x;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      t += Id<StringBuilder>(new StringBuilder("ab")).Length;
      var a := new Array<int>();
      a.Add(1);
      t += Id<Array<int>>(move a).Count;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
