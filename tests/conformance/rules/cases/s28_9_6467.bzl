//! case: s28_9_6467
//! doc: spec
//! section: 28.9
//! title: a function value bound from `??` and returned from an async method
//!   stays callable
//! rule: the type's lifetime is managed by the runtime without user
//!   involvement
//! cites: 7.7, 28.9
//! expect: run
//! stdout: 40200
module Conf;
type Fn := (int, int) -> int;
public class C
  public static async method Mk(k: int): Task<Fn>
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var n: Fn? := null;
    var b := n ?? g;
    return b;
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
