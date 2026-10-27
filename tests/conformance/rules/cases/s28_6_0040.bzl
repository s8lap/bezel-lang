//! case: s28_6_0040
//! doc: spec
//! section: 28.6
//! title: an alias of a using-var owner cannot dispose it
//! rule: "Dispose() or a direct DisposeAsync() call through a borrow is a compile
//!   error. The borrow receivers: a borrowed local or parameter" (§28.6, v0.340).
//!   `il` is an unmarked alias of the `using var` owner `a`, so it is a borrow of
//!   `a`, and `il.Dispose()` is disposal through a borrow — a compile error.
//!   The program also draws the spurious "`using var` `a` is vacuous" warning,
//!   because the ownership pass does not see that a collection literal is an
//!   owned production (§28.3's producers table). That warning is a separate
//!   defect, carried by the row `s28_10_0016` (issue #7092); this row asserts
//!   the §28.6 rejection only.
//! cites: 28.6, 28.3
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `il`
module Conf;

import System;

public class Program
  public static method Main()
    using var a: Array<int> := [1, 2, 3];
    var il: IList<int> := a;
    il.Dispose();
  end Main;
end Program;
