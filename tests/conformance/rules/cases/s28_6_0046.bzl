//! case: s28_6_0046
//! doc: spec
//! section: 28.6
//! title: a three-step alias chain cannot dispose the owner either
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: a borrowed local or parameter" (§28.6,
//!   v0.343), and "An **unmarked read of a class-typed binding never consumes**:
//!   `var b := a;` is always a borrow for class-typed `a`" (§28.3 rule 13,
//!   v0.343). `b` is a borrow of the owned `a`, `c` is a borrow of `b`, and `il`
//!   is a borrow of `c` — three hops of aliasing, every one of them a borrow, so
//!   `il.Dispose()` is disposal through a borrow: a compile error. The rule is
//!   structural: it reads the receiver expression's ownership classification,
//!   which §28.3 computes at every call site, so the depth of the chain does not
//!   change the verdict.
//! cites: 28.6, 28.3
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `il`
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var b: Array<int> := a;
    var c: Array<int> := b;
    var il: IList<int> := c;
    il.Dispose();
  end Main;
end Program;
