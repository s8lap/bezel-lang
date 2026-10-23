//! case: s28_6_0055
//! doc: spec
//! section: 28.6
//! title: an alias of a non-owned field read inside the declaring class cannot dispose it
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: ... a non-`owned` field read" (§28.6,
//!   v0.343), and "An **unmarked read of a class-typed binding never consumes**:
//!   `var b := a;` is always a borrow for class-typed `a`" (§28.3 rule 13,
//!   v0.343). The declaring class is not the exemption the `owned`-field rule
//!   gives (§28.6's owned-field read outside the declaring class is a borrow, and
//!   its inside-the-class counterpart is the only owned receiver): `V` is a
//!   non-`owned` field, so the read `V` inside `Holder` is a borrow all the
//!   same; `b`, an unmarked alias of that read, is a borrow of a borrow, still a
//!   borrow; `b.Dispose()` is disposal through a borrow: a compile error. KNOWN
//!   DIVERGENCE (#7097): the compiler admits the program — no diagnostic at all
//!   — and it double-frees at run time.
//! cites: 28.6, 28.3
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
  public method Bad()
    var b: Array<int> := V;
    b.Dispose();
  end Bad;
end Holder;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var h := new Holder(a);
    h.Bad();
  end Main;
end Program;
