//! case: s28_3_0320
//! doc: spec
//! section: 28.3
//! title: a ?? whose arm is a named owned local read and the other a fresh value is rejected
//! rule: borrowed ⊓ borrowed = **borrowed** with the union of sources; mixed = **rejected** —
//!   equalize the arms first
//! cites: 28.3.4, K3.13
//! expect: reject
//! diagnostic: mix an owned value with a borrowed
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Maybe(c: bool): owned StringBuilder?
    if c then return new StringBuilder("ab"); end if;
    return null;
  end Maybe;
  public static method Main(): int
    var x := Maybe(true);
    var r := x ?? new StringBuilder("xyz");
    return r.Length;
  end Main;
end C;
