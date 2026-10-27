//! case: s28_6_0057
//! doc: spec
//! section: 28.6
//! title: a using var bound over an alias cannot dispose it
//! rule: "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: a borrowed local or parameter" (§28.6,
//!   v0.343). The right-hand side of the `using var` is a borrow rather than an
//!   owned value, so the binding does not assume a disposal obligation: at scope
//!   end no `Dispose()` is called, because the value is not owned by this scope
//!   (§28.10, v0.343). `c` is therefore a borrow of `b`, which is itself a borrow
//!   of the owned `a`, and `c.Dispose()` is disposal through a borrow: a compile
//!   error.
//! cites: 28.6, 28.3, 28.10
//! expect: reject
//! diagnostic: cannot call `Dispose()` on `c`
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := new Array<int>(2);
    var b: Array<int> := a;
    using var c: Array<int> := b;
    c.Dispose();
  end Main;
end Program;
