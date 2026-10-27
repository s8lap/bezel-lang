//! case: s28_6_0044
//! doc: spec
//! section: 28.6
//! title: an unmarked alias of an owned local cannot dispose it
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: a borrowed local or parameter" (§28.6,
//!   v0.343), and "An **unmarked read of a class-typed binding never consumes**:
//!   `var b := a;` is always a borrow for class-typed `a`" (§28.3 rule 13,
//!   v0.343). `a` owns the collection literal it was bound to (§28.3's producers
//!   table), so `il` — an unmarked alias of `a` — is a borrow of `a` and takes no
//!   ownership of its own; `il.Dispose()` is disposal through a borrow: a compile
//!   error.
//! cites: 28.6, 28.3
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `il`
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var il: IList<int> := a;
    il.Dispose();
  end Main;
end Program;
