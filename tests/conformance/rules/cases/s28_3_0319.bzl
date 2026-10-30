//! case: s28_3_0319
//! doc: spec
//! section: 28.3
//! title: an OwningDictionary value read returned under an owned return is rejected
//! rule: Every return path supplies a fresh or owned value (`null` admissible under a nullable
//!   declared type
//! cites: 28.3.5, 28.11
//! expect: reject
//! diagnostic: borrow cannot be returned
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class W
  public n: int;
  public constructor(n: int) this.n := n; end constructor;
end W;
class C
  public static method Bad(o: OwningDictionary<int, OwningArray<W>>): owned OwningArray<W>
    return o[1];
  end Bad;
  public static method Main(): int
    return 0;
  end Main;
end C;
