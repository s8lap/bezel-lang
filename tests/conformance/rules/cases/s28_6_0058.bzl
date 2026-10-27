//! case: s28_6_0058
//! doc: spec
//! section: 28.6
//! title: an alias of two mutually aliased locals cannot dispose it
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: a borrowed local or parameter" (§28.6,
//!   v0.343), and "An **unmarked read of a class-typed binding never consumes**:
//!   `var b := a;` is always a borrow for class-typed `a`" (§28.3 rule 13,
//!   v0.343). Each of the two assignments is such an unmarked read, so neither
//!   binding becomes an owner by the other's; `z` is a borrow of a borrow, still a
//!   borrow, and `z.Dispose()` is disposal through a borrow: a compile error. The
//!   check is structural and decidable — it reads the receiver expression's
//!   ownership classification, which §28.3 computes at every call site (§28.6,
//!   v0.343).
//! cites: 28.6, 28.3
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `z`
module Conf;

import System;

public class Program
  public static method Main()
    var x: Array<int> := new Array<int>(2);
    var y: Array<int> := new Array<int>(3);
    x := y;
    y := x;
    var z: Array<int> := x;
    z.Dispose();
  end Main;
end Program;
