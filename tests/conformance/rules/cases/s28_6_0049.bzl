//! case: s28_6_0049
//! doc: spec
//! section: 28.6
//! title: an alias of a borrowed parameter cannot dispose it
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: a borrowed local or parameter" (§28.6,
//!   v0.343), and "An **unmarked read of a class-typed binding never consumes**:
//!   `var b := a;` is always a borrow for class-typed `a`" (§28.3 rule 13,
//!   v0.343). The parameter `p` is a borrowed parameter — the caller's argument
//!   is still the owner's — so `il`, an unmarked alias of `p`, is a borrow of a
//!   borrow, still a borrow; `il.Dispose()` is disposal through a borrow: a
//!   compile error.
//! cites: 28.6, 28.3
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `il`
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    Take(a);
  end Main;
  public static method Take(p: Array<int>)
    var il: IList<int> := p;
    il.Dispose();
  end Take;
end Program;
