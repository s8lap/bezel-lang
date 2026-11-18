//! case: s28_14_7648
//! doc: spec
//! section: 28.14
//! title: an async method keeps its call-bounded function parameter valid across its awaits
//! rule: lives until the task settles
//! cites: 28.14, 30.1
//! expect: run
//! stdout: 1200
module Conf;
import System;
class C
  public static async method Outer(h: (int) -> int): Task<int>
    var f := async lambda(n: int): Task<int> => do
      await Task.Delay(1);
      return n + 1;
    end;
    var x := await f(1);
    await Task.Delay(1);
    return x + h(2);
  end Outer;
  public static async method Main(): Task<int>
    var a := 0;
    for var k in 0..<200 do
      a := a + await Outer(lambda(v: int): int => v * 2);
    end;
    Console.WriteLine(a);
    return 0;
  end Main;
end C;
