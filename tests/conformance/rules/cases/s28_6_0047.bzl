//! case: s28_6_0047
//! doc: spec
//! section: 28.6
//! title: an alias of a move result cannot dispose it
//! rule: "Explicit disposal — `x.Dispose()` ... — requires a receiver the disposer
//!   owns: an owned local or `owned` parameter, a `using var`, an unbound owned
//!   temporary, a `move` result, or an `owned` field" (§28.6, v0.343). The `move`
//!   expression is the only owned read (§28.8.1), so `var b: Array<int> := move
//!   a;` makes `b` an owner and `a` moved-from; `b` is therefore a receiver the
//!   disposer owns and may be disposed. `il`, however, is an unmarked alias of
//!   `b` — "an unmarked read of a class-typed binding never consumes: `var b :=
//!   a;` is always a borrow for class-typed `a`" (§28.3 rule 13, v0.343) — so
//!   `il.Dispose()` is disposal through a borrow: a compile error.
//! cites: 28.6, 28.3, 28.8.1
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `il`
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var b: Array<int> := move a;
    var il: IList<int> := b;
    il.Dispose();
  end Main;
end Program;
