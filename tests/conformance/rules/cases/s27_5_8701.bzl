//! case: s27_5_8701
//! doc: spec
//! section: 27.5
//! title: a closure kept in a field sees a write to its captured local made after the keep
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9, 28.14
//! expect: run
//! stdout: 239700
//! notes: Pins #9086. `Fill` keeps `b` (captures `n`) in `box.F`, then writes `n` twice and
//!   reassigns the local `b` to a different closure. The kept closure still reads the shared
//!   variable: for k, n = k + 100 + 1000, so box.F(1, 2) = 1 - 2 + k + 1100 = k + 1099; summed
//!   over k = 0..199: 19900 + 200 * 1099 = 239700. The `run` lambda only assigns a lambda
//!   literal to a closure local of its own (the shape that exposed the defect). Twins:
//!   s27_5_8702 (two closures sharing one variable), s27_5_8703.
module Conf;
type Fn := (int, int) -> int;
public class Box
  public F: Fn := lambda(x: int, y: int) => 0;
end Box;

public class C
  public static method Fill(box: Box, k: int)
    var n := k;
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + n;
    var run := lambda(z: int): int => do
      var c: Fn := lambda(x: int, y: int) => x + z;
      c := lambda(x: int, y: int) => 0;
      return 0;
    end;
    _ := run(k);
    box.F := b;
    n := n + 100;
    b := g;
    n := n + 1000;
  end Fill;
  public static method Main(): int
    var total := 0;
    for var i in 0 ..< 200 do
      using var box: Box := new Box();
      Fill(box, i);
      total := total + box.F(1, 2);
    end for;
    Console.WriteLine($"{total}");
    return 0;
  end Main;
end C;
