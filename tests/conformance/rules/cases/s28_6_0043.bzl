//! case: s28_6_0043
//! doc: spec
//! section: 28.6
//! title: an alias of an owner that was assigned a collection literal cannot dispose it
//! rule: "Dispose() or a direct DisposeAsync() call through a borrow is a compile
//!   error. The borrow receivers: a borrowed local or parameter" (§28.6, v0.340),
//!   and "A collection literal (`[1, 2, 3]`) targeting a reference type is
//!   **Owned**" (§28.3's producers table). The assignment `a := [1, 2, 3];` makes
//!   `a` the owner of that literal, so `il` — an unmarked alias of `a` — is a
//!   borrow of it, and `il.Dispose()` is disposal through a borrow: a compile
//!   error.
//! cites: 28.6, 28.3
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `il`
//! issue: 7092
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := new Array<int>(2);
    a := [1, 2, 3];
    var il: IList<int> := a;
    il.Dispose();
  end Main;
end Program;
