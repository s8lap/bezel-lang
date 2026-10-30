//! case: s28_8_0008
//! doc: spec
//! section: 28.8
//! title: a for-in element variable passed to an owned parameter is rejected
//! rule: When a parameter is `owned`, the caller relinquishes ownership and may not use the argument
//!   after the call
//! cites: 22.3, 28.3.4
//! expect: reject
//! diagnostic: borrow cannot be passed
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Take(owned s: StringBuilder): int => s.Length;
  public static method Main(): int
    using var s := new StringBuilder("ab");
    var xs: Array<StringBuilder> := [s];
    var t := 0;
    for var x in xs do t += Take(x); end for;
    return t;
  end Main;
end C;
