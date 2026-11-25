//! case: s27_5_8911
//! doc: spec
//! section: 27.5
//! title: an async parameter captured in a `try` is read by the escaping closure after `return` and
//!   `finally`
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted
//!   (below).
//! cites: 27.5, 28.9, 30
//! expect: run
//! output: 40700
//! notes: Harvested from the #9126 review (p21). `Fill` captures `k` in `box.F` inside a `try`, awaits,
//!   returns 1 for even k from inside the `try` (otherwise awaits and returns 2 after it), and a
//!   `finally` stores a second closure over `k` into `box.G`. Per iteration: the returned 1 or 2, F(5,
//!   2) = 3 + i, G(0, 0) = i. Sum: 100 x 1 + 100 x 2 + 600 + 2 x 19900 = 40700. Value-only.
module Conf;
type Fn := (int, int) -> int;
public class Box
  public F: Fn := lambda(x: int, y: int) => 0;
  public G: Fn := lambda(x: int, y: int) => 0;
end Box;

public class C
  public static async method Fill(box: Box, k: int): Task<int>
    try
      box.F := lambda(x: int, y: int) => x - y + k;
      await Task.Delay(1);
      if k mod 2 = 0 then
        return 1;
      end if;
      await Task.Delay(0);
    finally
      box.G := lambda(x: int, y: int) => k;
    end try;
    return 2;
  end Fill;
  public static async method Main(): Task<int>
    var total := 0;
    for var i in 0 ..< 200 do
      using var box: Box := new Box();
      total := total + await Fill(box, i);
      total := total + box.F(5, 2) + box.G(0, 0);
    end for;
    Console.WriteLine($"{total}");
    return 0;
  end Main;
end C;
