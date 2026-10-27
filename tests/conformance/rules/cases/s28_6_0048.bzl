//! case: s28_6_0048
//! doc: spec
//! section: 28.6
//! title: a two-step alias chain over a reassigned owner cannot dispose it
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: a borrowed local or parameter" (§28.6,
//!   v0.343), and "An **unmarked read of a class-typed binding never consumes**:
//!   `var b := a;` is always a borrow for class-typed `a`" (§28.3 rule 13,
//!   v0.343). `b` is an unmarked alias of the owned `a`, and `il` is an unmarked
//!   alias of `b` — a borrow of a borrow, still a borrow. The reassignment
//!   `a := [1, 2, 3];` makes `a` the owner of the literal it stores (§28.3's
//!   producers table) without making `b` or `il` owners, so `il.Dispose()` is
//!   still disposal through a borrow: a compile error.
//! cites: 28.6, 28.3, 28.7
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `il`
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := new Array<int>(2);
    var b: Array<int> := a;
    a := [1, 2, 3];
    var il: IList<int> := b;
    il.Dispose();
  end Main;
end Program;
