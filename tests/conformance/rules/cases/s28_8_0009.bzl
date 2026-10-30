//! case: s28_8_0009
//! doc: spec
//! section: 28.8
//! title: a pattern-bound record component passed to an owned parameter is rejected
//! rule: When a parameter is `owned`, the caller relinquishes ownership and may not use the argument
//!   after the call
//! cites: 22.3, 28.3.4
//! expect: reject
//! diagnostic: borrow cannot be passed
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
record Box(sb: StringBuilder);
class C
  public static method Take(owned s: StringBuilder): int => s.Length;
  public static method Bad(b: Box): int
    return (case b of Box(s) => Take(s));
  end Bad;
  public static method Main(): int
    return 0;
  end Main;
end C;
