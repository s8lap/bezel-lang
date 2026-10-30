//! case: s28_6_0072
//! doc: spec
//! section: 28.6
//! title: a borrowed array element added to an OwningArray slot is rejected
//! rule: A **borrow** stored into an owned location — an `owned` field, an owning-collection slot,
//!   or an owned-state binding
//! cites: 28.6, 28.11
//! expect: reject
//! diagnostic: borrow cannot be
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Bad2(o: OwningArray<StringBuilder>, a: Array<StringBuilder>)
    o.Add(a[0]);
  end Bad2;
  public static method Main(): int
    return 0;
  end Main;
end C;
