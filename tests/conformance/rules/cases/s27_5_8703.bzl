//! case: s27_5_8703
//! doc: spec
//! section: 27.5
//! title: a closure kept by an inner lambda into a second box keeps its own captured parameter
//!   alive
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter
//!   excepted (below).
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 41400
//! notes: Pins #9086. The lambda `run` keeps its local closure `c` (which captures the lambda's
//!   parameter `z`) in `b2.F` and then reassigns the local `c`; `b` is kept in `b1.F` and its
//!   local reassigned. Both kept closures read their captured variable after `Fill` returned:
//!   b1.F(5, 2) = 5 - 2 + k and b2.F(5, 2) = 5 + k, i.e. 8 + 2k per iteration; summed over k =
//!   0..199: 1600 + 39800 = 41400.
module Conf;
type Fn := (int, int) -> int;
public class Box
  public F: Fn := lambda(x: int, y: int) => 0;
end Box;

public class C
  public static method Fill(b1: Box, b2: Box, k: int)
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
    var run := lambda(z: int): int => do
      var c: Fn := lambda(x: int, y: int) => x + z;
      b2.F := c;
      c := lambda(x: int, y: int) => 0;
      return 0;
    end;
    _ := run(k);
    b1.F := b;
    b := g;
  end Fill;
  public static method Main(): int
    var total := 0;
    for var i in 0 ..< 200 do
      using var b1: Box := new Box();
      using var b2: Box := new Box();
      Fill(b1, b2, i);
      total := total + b1.F(5, 2) + b2.F(5, 2);
    end for;
    Console.WriteLine($"{total}");
    return 0;
  end Main;
end C;
