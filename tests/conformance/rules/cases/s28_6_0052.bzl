//! case: s28_6_0052
//! doc: spec
//! section: 28.6
//! title: an interface-typed alias hop cannot dispose the owner
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: a borrowed local or parameter" (§28.6,
//!   v0.343), and "An **unmarked read of a class-typed binding never consumes**:
//!   `var b := a;` is always a borrow for class-typed `a`" (§28.3 rule 13,
//!   v0.343). The aliasing is not undone by the declared type of the alias: `il`
//!   is an unmarked alias of the owned `a` and is a borrow of it, and `il2`, an
//!   unmarked alias of `il`, is a borrow of a borrow — an interface-typed hop is
//!   still a borrow, so `il2.Dispose()` is disposal through a borrow: a compile
//!   error.
//! cites: 28.6, 28.3
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `il2`
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var il: IList<int> := a;
    var il2: IList<int> := il;
    il2.Dispose();
  end Main;
end Program;
