//! case: s28_3_0317
//! doc: spec
//! section: 28.3
//! title: a ?? mixing a borrowed parameter with a fresh value is rejected
//! rule: borrowed ⊓ borrowed = **borrowed** with the union of sources; mixed = **rejected** —
//!   equalize the arms first
//! cites: 28.3.4, K7.3
//! expect: reject
//! diagnostic: mix an owned value with a borrowed
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Bad(p: StringBuilder?): int
    var r := p ?? new StringBuilder("n");
    return r.Length;
  end Bad;
  public static method Main(): int
    return 0;
  end Main;
end C;
