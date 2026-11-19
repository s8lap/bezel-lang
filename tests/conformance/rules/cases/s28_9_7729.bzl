//! case: s28_9_7729
//! doc: spec
//! section: 28.9
//! title: a function-typed getter may return a closure over a value computed from a lambda local
//! rule: a getter never transfers a reclamation obligation to its reader
//! cites: 23.3, 27.5
//! expect: run
//! stdout: 3
module Conf;
import System;
class C
  public property F: () -> int
    get => do
      var a: Array<int> := [1, 2, 3];
      var g := lambda(): int => a.Count;
      var n := g();
      return lambda(): int => n;
    end;
  end F;
end C;
class P
  public static method Main(): int
    var c := new C();
    var f := c.F;
    Console.WriteLine(f());
    return 0;
  end Main;
end P;
