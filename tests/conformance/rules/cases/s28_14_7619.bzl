//! case: s28_14_7619
//! doc: spec
//! section: 28.14
//! title: a closure local reassigned between two calls at a function-typed parameter is used by the later call
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.9
//! expect: run
//! stdout: 5800
module Conf;
import System;
class C
  public static method Run(): int
    var m := 3;
    var k := lambda() => m;
    var h := lambda(g: () -> int) => g();
    var a := h(k);
    k := lambda() => m + 10;
    var b := h(k);
    return a + b + k();
  end Run;
  public static method Main(): int
    var s := 0;
    for var i in 0 ..< 200 do
      s := s + Run();
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
