//! case: s28_14_7649
//! doc: spec
//! section: 28.14
//! title: a nested async lambda calling the enclosing call-bounded parameter gives the hand total
//! rule: lives until the task settles
//! cites: 28.14, 30.1
//! expect: run
//! stdout: 600
module Conf;
import System;
class C
  public static async method Outer(h: (int) -> int): Task<int>
    var f := async lambda(n: int): Task<int> => do
      await Task.Delay(1);
      return h(n) + 1;
    end;
    return await f(1);
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
