//! case: s28_9_7728
//! doc: spec
//! section: 28.9
//! title: a block getter may bind an array local and call a lambda over it
//! rule: a getter never transfers a reclamation obligation to its reader
//! cites: 23.3, 27.5
//! expect: run
//! stdout: 6
module Conf;
import System;
class C
  public property N: int
    get => do
      var a: Array<int> := [1, 2, 3];
      var f := lambda() => a.Count + a[2];
      return f();
    end;
  end N;
end C;
class P
  public static method Main(): int
    var c := new C();
    Console.WriteLine(c.N);
    return 0;
  end Main;
end P;
