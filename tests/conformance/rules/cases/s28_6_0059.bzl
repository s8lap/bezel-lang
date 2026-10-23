//! case: s28_6_0059
//! doc: spec
//! section: 28.6
//! title: an alias of a conditionally assigned owner cannot dispose it
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: a borrowed local or parameter" (§28.6,
//!   v0.343), and "An **unmarked read of a class-typed binding never consumes**:
//!   `var b := a;` is always a borrow for class-typed `a`" (§28.3 rule 13,
//!   v0.343). Whichever arm of the `if` runs, `a` is a named binding, so the
//!   unmarked assignment `il := a` is a borrow of `a` — the alias never becomes an
//!   owner, and the conditional does not change that: `il.Dispose()` is disposal
//!   through a borrow: a compile error.
//! cites: 28.6, 28.3, 20
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `il`
module Conf;

import System;

public class Program
  public static method Main()
    var owner: Array<int> := new Array<int>(2);
    var a: Array<int>;
    if owner.Count > 1 then
      a := owner;
    else
      a := [1, 2, 3];
    end;
    var il: IList<int> := a;
    il.Dispose();
  end Main;
end Program;
