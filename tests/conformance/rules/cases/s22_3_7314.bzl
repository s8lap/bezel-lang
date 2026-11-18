//! case: s22_3_7314
//! doc: spec
//! section: 22.3
//! title: a lambda body writing `move` on its borrowed parameter at an
//!   `owned` argument is rejected
//! rule: writing it on a borrow, a fresh producer, or a non-`owned` parameter
//!   position is likewise an error
//! cites: 28.8
//! expect: reject
//! diagnostic: borrow
//! notes: Control for the #8797 fix, green before and after: the `move l`
//!   marker names a plain lambda parameter, which is a borrow and not a named
//!   owned binding. The method-body twin is rejected the same way.
module Conf;

class C
  public static method Kill(owned a: Array<int>): int
    var n := a.Count;
    a.Dispose();
    return n;
  end Kill;

  public static method Main(): int
    var xs: Array<int> := [1, 2, 3];
    var f := lambda(l: Array<int>): int => do var k := Kill(move l); return k; end;
    Console.WriteLine(f(xs));
    return 0;
  end Main;
end C;
