//! case: s28_6_7311
//! doc: spec
//! section: 28.6
//! title: a nested lambda storing its borrowed parameter into `this`'s owned
//!   field is rejected
//! rule: A **borrow** stored into an owned location — an `owned` field, an
//!   owning-collection slot, or an owned-state binding
//! cites: 28.5, 28.3
//! expect: reject
//! diagnostic: owned
//! notes: Pins the #8797 fix through two lambda levels: the store `this.saved
//!   := m` is in the inner lambda, whose parameter `m` is a borrow. Control
//!   for the accepting twin: an inner `this.saved := new Array<int>()` is
//!   admitted.
module Conf;

class C
  private owned saved: Array<int>?;

  public method Go(): int
    var xs: Array<int> := [1, 2, 3];
    var f := lambda(l: Array<int>): int => do
      var g := lambda(m: Array<int>) => do this.saved := m; end;
      g(l);
      return l.Count;
    end;
    return f(xs);
  end Go;

  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c.Go());
    return 0;
  end Main;
end C;
