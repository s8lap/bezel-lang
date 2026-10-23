//! case: s28_6_0042
//! doc: spec
//! section: 28.6
//! title: a two-step alias chain cannot dispose the owner either
//! rule: "Dispose() or a direct DisposeAsync() call through a borrow is a compile
//!   error. The borrow receivers: a borrowed local or parameter" (§28.6, v0.340).
//!   `b` is an unmarked alias of the owned `a`, so it is a borrow of `a`; `il` is
//!   an unmarked alias of `b`, so it is a borrow of a borrow — still a borrow, and
//!   `il.Dispose()` is disposal through a borrow, which is a compile error. The
//!   rule is structural: it reads the receiver expression's ownership
//!   classification, which §28.3 computes at every call site.
//! cites: 28.6, 28.3
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `il`
//! issue: 7091
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var b: Array<int> := a;
    var il: IList<int> := b;
    il.Dispose();
  end Main;
end Program;
