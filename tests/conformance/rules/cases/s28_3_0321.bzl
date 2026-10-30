//! case: s28_3_0321
//! doc: spec
//! section: 28.3
//! title: a fluent call over a borrowed local alias returned under an owned return is rejected
//! rule: Every return path supplies a fresh or owned value (`null` admissible under a nullable
//!   declared type
//! cites: 28.3.5, 28.3.4
//! expect: reject
//! diagnostic: borrow cannot be returned
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Bad(p: StringBuilder): owned StringBuilder
    var b := p;
    return b.Append("x");
  end Bad;
  public static method Main(): int
    return 0;
  end Main;
end C;
