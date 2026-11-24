//! case: s28_14_8103
//! doc: spec
//! section: 28.14
//! title: the caller's closure stays usable after being passed through a const alias in a
//!  call-bounded callee
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as
//!  an argument to another call-bounded parameter
//! cites: 28.14
//! expect: run
//! output: 9
//! notes: Pins #8983: the callee's alias must not release the caller's closure, so the
//!  caller's later reads through k2 still see m = 3; 3 + 3 + 3 = 9.
module Conf;

class C
  public static method Id(g: () -> int): int
    return g();
  end Id;
  public static method F1(g: () -> int): int
    const h := g;
    return C.Id(h);
  end F1;
  public static method Main(): int
    var m := 3;
    var k := lambda() => m;
    var a := C.F1(k);
    var k2 := k;
    Console.WriteLine(a + C.F1(k2) + k2());
    return 0;
  end Main;
end C;
