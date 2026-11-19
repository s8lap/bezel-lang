//! case: s28_14_7715
//! doc: spec
//! section: 28.14
//! title: a call-bounded parameter bound to a local may be forwarded on and the closure used afterwards
//! rule: the callback may be invoked, bound to locals, and passed on as an argument to another call-bounded parameter
//! cites: 28.3
//! expect: run
//! stdout: 9
module Conf;
import System;
public class C
  public static method Id(g: () -> int): int
    return g();
  end Id;
  public static method F1(g: () -> int): int
    var h := g;
    return C.Id(h) + C.Id(h);
  end F1;
  public static method Main(): int
    var m := 3;
    var k := lambda() => m;
    var r := C.F1(k);
    Console.WriteLine(r + k());
    return 0;
  end Main;
end C;
