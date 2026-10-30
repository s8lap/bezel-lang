//! case: s28_3_0323
//! doc: spec
//! section: 28.3
//! title: a ?? of a borrowed parameter and a fresh value under an owned return is rejected
//! rule: borrowed ⊓ borrowed = **borrowed** with the union of sources; mixed = **rejected** —
//!   equalize the arms first
//! cites: 28.3.4, 28.3.5
//! expect: reject
//! diagnostic: mix an owned value with a borrowed
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Bad(r: Random?): owned Random
    return r ?? new Random(1);
  end Bad;
  public static method Main(): int
    return 0;
  end Main;
end C;
