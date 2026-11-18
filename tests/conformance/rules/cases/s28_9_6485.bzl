//! case: s28_9_6485
//! doc: spec
//! section: 28.9
//! title: an owned alias returned at an Object? result stays callable
//! rule: the type's lifetime is managed by the runtime without user
//!   involvement
//! cites: 28.9, 8.6
//! expect: run
//! stdout: 40200
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Mk(k: int): Object?
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    return a;
  end Mk;
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      var o := Mk(i);
      var f := o as Fn;
      total := total + f!(1, 2) + f!(4, 1);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
