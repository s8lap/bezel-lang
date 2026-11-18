//! case: s28_9_6478
//! doc: spec
//! section: 28.9
//! title: a captured alias that is never returned keeps its source alive
//!   across the call
//! rule: the type's lifetime is managed by the runtime without user
//!   involvement
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 20300
module Conf;
public class C
  public static Acc: int := 0;
  public static method Mk(k: int, c: bool): (int, int) -> int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := g;
    var h := lambda(): int => cmp(3, 1);
    C.Acc := C.Acc + h() + k;
    return g;
  end Mk;
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      var f := C.Mk(i, false);
      total := total + f(3, 1) + f(1, 3);
    end for;
    Console.WriteLine(C.Acc + total);
    return 0;
  end Main;
end C;
