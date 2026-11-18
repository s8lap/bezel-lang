//! case: s28_14_7618
//! doc: spec
//! section: 28.14
//! title: a closure local passed repeatedly at a function-typed parameter of a lambda stays usable
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.9
//! expect: run
//! stdout: 1200
module Conf;
import System;
class C
  public static method Run(): int
    var m := 3;
    var k := lambda() => m;
    var h := lambda(g: () -> int) => g();
    return h(k) + h(k);
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
