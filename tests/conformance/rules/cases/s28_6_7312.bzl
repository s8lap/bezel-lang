//! case: s28_6_7312
//! doc: spec
//! section: 28.6
//! title: a lambda body storing a borrow through a compound target
//!   `b.h.saved` into an owned field is rejected
//! rule: A **borrow** stored into an owned location — an `owned` field, an
//!   owning-collection slot, or an owned-state binding
//! cites: 28.5, 28.3
//! expect: reject
//! diagnostic: owned
//! notes: Pins the #8797 fix for a store whose target is a path
//!   (`b.h.saved`): the owned field is reached through two borrows. The
//!   borrowed parameter `l` still lands in an owned location.
module Conf;

class Holder
  public owned saved: Array<int>?;
end Holder;

class Box
  public h: Holder;

  public constructor(h: Holder)
    this.h := h;
  end constructor;
end Box;

class C
  public static method Main(): int
    using var h := new Holder();
    var b := new Box(h);
    var xs: Array<int> := [1, 2, 3];
    var f := lambda(l: Array<int>) => do b.h.saved := l; end;
    f(xs);
    Console.WriteLine(xs.Count);
    return 0;
  end Main;
end C;
