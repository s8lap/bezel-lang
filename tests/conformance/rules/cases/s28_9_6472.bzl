//! case: s28_9_6472
//! doc: spec
//! section: 28.9
//! title: a function value returned through a two-deep `??` stays callable
//! rule: the type's lifetime is managed by the runtime without user
//!   involvement
//! cites: 7.7, 28.9
//! expect: run
//! stdout: 40200
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Mk(k: int): Fn
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var n: Fn? := null;
    var m: Fn? := null;
    return n ?? (m ?? g);
  end Mk;
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      var f := Mk(i);
      total := total + f(1, 2) + f(4, 1);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
