//! case: s28_8_0007
//! doc: spec
//! section: 28.8
//! title: a non-owned Random field read passed to an owned parameter is rejected
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
  public r: Random;
  public constructor(r: Random) this.r := r; end constructor;
  public method Bad()
    C.TakeR(this.r);
  end Bad;
end H;
class C
  public static method TakeR(owned r: Random)
    Console.WriteLine(r.Next(1));
  end TakeR;
  public static method Main(): int
    return 0;
  end Main;
end C;
