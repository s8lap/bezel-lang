//! case: s28_14_8102
//! doc: spec
//! section: 28.14
//! title: a const-bound call-bounded callback may be invoked and passed on twice
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as
//!  an argument to another call-bounded parameter
//! cites: 28.14
//! expect: run
//! output: 12
//! notes: Pins #8983 (const spelling of the alias). Each F1 call yields 3 + 3 = 6, twice.
module Conf;

class C
  public static method Id(g: () -> int): int
    return g();
  end Id;
  public static method F1(g: () -> int): int
    const h := g;
    return C.Id(h) + h();
  end F1;
  public static method Main(): int
    var m := 3;
    var k := lambda() => m;
    Console.WriteLine(C.F1(k) + C.F1(k));
    return 0;
  end Main;
end C;
