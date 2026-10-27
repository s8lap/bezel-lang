//! case: s28_6_0054
//! doc: spec
//! section: 28.6
//! title: a using var alias of a non-owned field read cannot dispose it
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: ... a non-`owned` field read" (§28.6,
//!   v0.343), and "An **unmarked read of a class-typed binding never consumes**:
//!   `var b := a;` is always a borrow for class-typed `a`" (§28.3 rule 13,
//!   v0.343). `h.V` is a non-`owned` field read, so it is a borrow of the field's
//!   value, and a `using var` binding adopts a borrow as a borrow (§28.10) and
//!   takes no disposal obligation of its own; `b.Dispose()` is disposal through
//!   a borrow: a compile error. KNOWN DIVERGENCE (#7097): the compiler admits
//!   the program, drawing only the §28.9 "vacuous" warning, and it double-frees
//!   at run time.
//! cites: 28.6, 28.3, 28.10
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `b`
//! issue: 7097
module Conf;

import System;

public class Holder
  public V: Array<int>;
  public constructor(v: Array<int>)
    V := v;
  end constructor;
end Holder;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var h := new Holder(a);
    using var b: Array<int> := h.V;
    b.Dispose();
  end Main;
end Program;
