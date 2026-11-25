//! case: s27_5_8702
//! doc: spec
//! section: 27.5
//! title: two closures kept in two fields share one captured variable after the declaring scope
//!   ends
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 60500
//! notes: Pins #9086. `inc` and `get` capture the one variable `n`; they are kept in two fields and
//!   the locals are then reassigned. The kept closures still share `n`. `Fill` leaves n = k.
//!   The three statements of `Main` run in order: b1.F -> inc: n = k + 1, returns k + 1; b2.F
//!   -> get: k + 1; b1.F -> inc: n = k + 2, returns k + 2: 3k + 4 per iteration; summed over k
//!   = 0..199: 3 * 19900 + 800 = 60500.
module Conf;
type Fn := (int, int) -> int;
public class Box
  public F: Fn := lambda(x: int, y: int) => 0;
end Box;

public class C
  public static method Fill(b1: Box, b2: Box, k: int)
    var n := k;
    var g: Fn := lambda(x: int, y: int) => 0;
    var inc: Fn := lambda(x: int, y: int) => do n := n + 1; return n; end;
    var get: Fn := lambda(x: int, y: int) => n;
    var run := lambda(z: int): int => do
      var c: Fn := lambda(x: int, y: int) => x + z;
      c := lambda(x: int, y: int) => 0;
      return 0;
    end;
    _ := run(k);
    b1.F := inc;
    b2.F := get;
    inc := g;
    get := g;
  end Fill;
  public static method Main(): int
    var total := 0;
    for var i in 0 ..< 200 do
      using var b1: Box := new Box();
      using var b2: Box := new Box();
      Fill(b1, b2, i);
      var a := b1.F(0, 0);
      var b := b2.F(0, 0);
      var c := b1.F(0, 0);
      total := total + a + b + c;
    end for;
    Console.WriteLine($"{total}");
    return 0;
  end Main;
end C;
