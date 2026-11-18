//! case: s28_9_6481
//! doc: spec
//! section: 28.9
//! title: an owned alias returned from an async method stays callable
//! rule: the type's lifetime is managed by the runtime without user
//!   involvement
//! cites: 28.9, 30.1
//! expect: run
//! stdout: 40200
module Conf;
public class C
  public static async method Mk(k: int): Task<(int, int) -> int>
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    return a;
  end Mk;
  public static async method Main(): Task<int>
    var total := 0;
    for var i in 0..<200 do
      var f := await Mk(i);
      total := total + f(1, 2) + f(4, 1);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
