//! case: s28_9_6471
//! doc: spec
//! section: 28.9
//! title: a function value stored in a field through a two-deep `??` stays
//!   callable
//! rule: the type's lifetime is managed by the runtime without user
//!   involvement
//! cites: 7.7, 28.9
//! expect: run
//! stdout: 40200
module Conf;
type Fn := (int, int) -> int;
public class H
  public F: Fn := lambda(x: int, y: int) => 0;
end H;
public class C
  public static method Fill(h: H, k: int)
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var n: Fn? := null;
    var m: Fn? := null;
    h.F := n ?? (m ?? g);
  end Fill;
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      var h := new H();
      Fill(h, i);
      total := total + h.F(1, 2) + h.F(4, 1);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
