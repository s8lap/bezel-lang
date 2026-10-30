//! case: s28_8_0006
//! doc: spec
//! section: 28.8
//! title: a non-owned field read passed to an owned parameter is rejected
//! rule: When a parameter is `owned`, the caller relinquishes ownership and may not use the argument
//!   after the call
//! cites: 22.3, 28.3.4
//! expect: reject
//! diagnostic: borrow cannot be passed
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class H
  public sb: StringBuilder;
  public constructor(s: StringBuilder) this.sb := s; end constructor;
  public method Bad()
    C.Take(this.sb);
  end Bad;
end H;
class C
  public static method Take(owned s: StringBuilder)
    Console.WriteLine(s.Length);
  end Take;
  public static method Main(): int
    return 0;
  end Main;
end C;
