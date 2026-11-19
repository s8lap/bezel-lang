//! case: s28_14_7713
//! doc: spec
//! section: 28.14
//! title: a call-bounded parameter may be forwarded on and the closure used afterwards
//! rule: the callback may be invoked, bound to locals, and passed on as an argument to another call-bounded parameter
//! cites: 28.3
//! expect: run
//! stdout: 6
module Conf;
import System;
public class C
  public static method Id(g: () -> int): int
    return g();
  end Id;
  public static method F1(g: () -> int): int
    return C.Id(g);
  end F1;
  public static method Main(): int
    var m := 3;
    var k := lambda() => m;
    Console.WriteLine(C.F1(k) + k());
    return 0;
  end Main;
end C;
