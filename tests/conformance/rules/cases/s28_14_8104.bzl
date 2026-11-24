//! case: s28_14_8104
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback bound to a local may not be returned through the alias
//! rule: returned, captured by a closure that itself escapes the callee (judged by these
//!  same rules, applied transitively)
//! cites: 28.14
//! expect: reject
//! diagnostic: call-bounded
//! notes: Pins the rejection side of #8983: binding the callback to a local does not
//!  launder it, a return of the alias is a return of the callback. Admitting twin:
//!  s28_14_8101 (alias invoked and forwarded).
module Conf;

class C
  public static method Id(g: () -> int): int
    return g();
  end Id;
  public static method F1(g: () -> int): () -> int
    var h := g;
    return h;
  end F1;
  public static method Main(): int
    var m := 3;
    var k := lambda() => m;
    var z := C.F1(k);
    Console.WriteLine(z());
    return 0;
  end Main;
end C;
