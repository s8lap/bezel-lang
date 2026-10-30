//! case: s28_6_0071
//! doc: spec
//! section: 28.6
//! title: a non-owned field read stored into an owned field is rejected
//! rule: A **borrow** stored into an owned location — an `owned` field, an owning-collection slot,
//!   or an owned-state binding
//! cites: 28.6, 28.5
//! expect: reject
//! diagnostic: borrow cannot be stored
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class H
  public sb: StringBuilder;
  private owned _o: StringBuilder;
  public constructor(s: StringBuilder)
    this.sb := s;
    this._o := new StringBuilder("q");
  end constructor;
  public method Bad()
    this._o := this.sb;
  end Bad;
end H;
class C
  public static method Main(): int
    return 0;
  end Main;
end C;
