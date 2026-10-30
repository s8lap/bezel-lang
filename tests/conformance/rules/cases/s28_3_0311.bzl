//! case: s28_3_0311
//! doc: spec
//! section: 28.3
//! title: owned meet owned in a ?? returned under an owned return is accepted
//! rule: Every return path supplies a fresh or owned value (`null` admissible under a nullable
//!   declared type
//! cites: 28.3.4, K7.3
//! expect: run
//! output: 250
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Maybe(c: bool): owned StringBuilder?
    if c then return new StringBuilder("ab"); end if;
    return null;
  end Maybe;
  public static method Mk(c: bool): owned StringBuilder
    return Maybe(c) ?? new StringBuilder("xyz");
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do t += Mk(i mod 2 = 0).Length; end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
