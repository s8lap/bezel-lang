//! case: s28_3_0310
//! doc: spec
//! section: 28.3
//! title: a range-variable slice returned under an owned return is accepted
//! rule: Owned — a fresh `Array<T>`, the compiler-recognized `Slice(r: Range<int>): owned Array<T>`
//!   form
//! cites: 28.3.4, 15.6
//! expect: run
//! output: 200
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Mk(a: Array<StringBuilder>, r: Range<int>): owned Array<StringBuilder>
    return a[r];
  end Mk;
  public static method Main(): int
    using var s := new StringBuilder("ab");
    var xs: Array<StringBuilder> := [s, s, s];
    var t := 0;
    for var i in 0..<100 do t += Mk(xs, 0..<2).Count; end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
