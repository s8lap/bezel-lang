//! case: s28_6_7313
//! doc: spec
//! section: 28.6
//! title: a lambda body storing its borrowed parameter into `this`'s non-null
//!   owned field is rejected
//! rule: A **borrow** stored into an owned location — an `owned` field, an
//!   owning-collection slot, or an owned-state binding
//! cites: 28.5, 28.3
//! expect: reject
//! diagnostic: owned
//! notes: Pins the #8797 fix for a lambda defined in an instance method of
//!   the class whose owned field is stored. The field is non-nullable and
//!   initialised in the constructor, so the only error ground is the store of
//!   the borrow `l`.
module Conf;

class C
  private owned saved: Array<int>;

  public constructor()
    this.saved := new Array<int>();
  end constructor;

  public method Go(): int
    var xs: Array<int> := [1, 2, 3];
    var f := lambda(l: Array<int>): int => do this.saved := l; return l.Count; end;
    return f(xs);
  end Go;

  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c.Go());
    return 0;
  end Main;
end C;
