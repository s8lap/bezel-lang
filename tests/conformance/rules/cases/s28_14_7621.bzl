//! case: s28_14_7621
//! doc: spec
//! section: 28.14
//! title: a lambda returning a closure that does not capture its function-typed parameter is admitted
//! rule: captured by a closure that itself escapes the callee (judged by these same rules, applied transitively)
//! cites: 28.3.8
//! expect: run
//! stdout: 1
module Conf;
import System;
class C
  public static method Run(): int
    var m := 3;
    var k := lambda() => m;
    var h := lambda(g: () -> int) => lambda() => 1;
    var r := h(k);
    return r();
  end Run;
  public static method Main(): int
    Console.WriteLine(Run());
    return 0;
  end Main;
end C;
