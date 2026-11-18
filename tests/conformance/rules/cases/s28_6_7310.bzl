//! case: s28_6_7310
//! doc: spec
//! section: 28.6
//! title: a lambda body storing its borrowed parameter into another object's
//!   owned field is rejected
//! rule: A **borrow** stored into an owned location — an `owned` field, an
//!   owning-collection slot, or an owned-state binding
//! cites: 28.5, 28.3
//! expect: reject
//! diagnostic: owned
//! notes: Pins the #8797 fix. The lambda parameter `l` is a borrow (the
//!   lambda has no `owned` marker on it), `h.saved` is an `owned` field of
//!   another object, and the store sits in the lambda body. The method-body
//!   twin of this store was already rejected. The accepting twin stores a
//!   fresh `new Array<int>()`, as s28_6_7315 does.
module Conf;

class Holder
  public owned saved: Array<int>?;
end Holder;

class C
  public static method Main(): int
    using var h := new Holder();
    var xs: Array<int> := [1, 2, 3];
    var f := lambda(l: Array<int>) => do h.saved := l; end;
    f(xs);
    Console.WriteLine(xs.Count);
    return 0;
  end Main;
end C;
