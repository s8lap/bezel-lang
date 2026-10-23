//! case: s28_6_0056
//! doc: spec
//! section: 28.6
//! title: a const alias of an owner cannot dispose it
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: a borrowed local or parameter" (§28.6,
//!   v0.343), and "An **unmarked read of a class-typed binding never consumes**:
//!   `var b := a;` is always a borrow for class-typed `a`" (§28.3 rule 13,
//!   v0.343). The `const` binding reads `b` without a marker, so `c` is a borrow of
//!   `b` — and `b` is itself a borrow of the owned `a` — whatever the binding's
//!   constness: `c.Dispose()` is disposal through a borrow: a compile error.
//! cites: 28.6, 28.3
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `c`
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := new Array<int>(2);
    var b: Array<int> := a;
    const c: Array<int> := b;
    c.Dispose();
  end Main;
end Program;
