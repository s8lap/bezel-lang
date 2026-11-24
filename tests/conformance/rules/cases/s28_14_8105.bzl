//! case: s28_14_8105
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback bound to a local may not be forwarded into a scoped
//!  parameter
//! rule: or forwarded into a `scoped` parameter position: the callee cannot see its
//!  argument's capture set, so it cannot vouch
//! cites: 28.14
//! expect: reject
//! diagnostic: scoped
//! notes: Pins #8983's rejection boundary: an alias of a call-bounded formal, through two
//!  locals, forwarded to a scoped parameter of an instance method (the receiver is the
//!  bounding object). Admitting twin: s28_14_8101 forwards to a call-bounded parameter.
module Conf;

class Bounder
  public method Id2(scoped g: () -> int): int
    return g();
  end Id2;
end Bounder;
class C
  public static method F1(g: () -> int): int
    var h := g;
    var j := h;
    using var b := new Bounder();
    return b.Id2(j);
  end F1;
  public static method Main(): int
    var m := 3;
    var k := lambda() => m;
    Console.WriteLine(C.F1(k));
    return 0;
  end Main;
end C;
