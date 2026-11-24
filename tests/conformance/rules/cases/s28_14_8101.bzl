//! case: s28_14_8101
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback bound to a local may be invoked and passed on
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as
//!  an argument to another call-bounded parameter
//! cites: 28.14
//! expect: run
//! output: 6
//! notes: Pins the admitted shapes of #8983 (alias of a call-bounded formal): a=3 through
//!  the alias, b=3 through the caller's own closure, sum 6.
module Conf;

class C
  public static method Id(g: () -> int): int
    return g();
  end Id;
  public static method F1(g: () -> int): int
    var h := g;
    return C.Id(h);
  end F1;
  public static method Main(): int
    var m := 3;
    var k := lambda() => m;
    var a := C.F1(k);
    var b := k();
    Console.WriteLine(a + b);
    return 0;
  end Main;
end C;
