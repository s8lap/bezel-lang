//! case: s28_6_0051
//! doc: spec
//! section: 28.6
//! title: a non-owned field read cannot be disposed through
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: ... a non-`owned` field read" (§28.6,
//!   v0.343). `V` is a non-`owned` field, so `h.V` is a borrow of the field's
//!   value and not an owned receiver; the field's actual owner disposes it.
//!   `h.V.Dispose()` is therefore disposal through a borrow: a compile error.
//! cites: 28.6
//! expect: reject
//! diagnostic: through a field read
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
    h.V.Dispose();
  end Main;
end Program;
