//! case: s28_6_0050
//! doc: spec
//! section: 28.6
//! title: an alias of an owned field read from outside its declaring class cannot dispose it
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: ... an owned-field read outside the
//!   declaring class" (§28.6, v0.343), and "An **unmarked read of a class-typed
//!   binding never consumes**: `var b := a;` is always a borrow for class-typed
//!   `a`" (§28.3 rule 13, v0.343). `h.V` is an owned-field read made from
//!   outside `Holder`, so it is a borrow of the field; `b`, an unmarked alias of
//!   that read, is a borrow of a borrow, still a borrow; `b.Dispose()` is
//!   disposal through a borrow: a compile error.
//! cites: 28.6, 28.3
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `b`
module Conf;

import System;

public class Holder
  public owned V: Array<int>;
  public constructor()
    V := [1, 2, 3];
  end constructor;
end Holder;

public class Program
  public static method Main()
    var h := new Holder();
    var b: Array<int> := h.V;
    b.Dispose();
  end Main;
end Program;
