//! case: s27_5_8907
//! doc: spec
//! section: 27.5
//! title: an escaping closure over an async parameter sees the writes made after later awaits
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice versa.
//! cites: 27.5, 28.9, 30
//! expect: run
//! output: 220700
//! notes: Harvested from the #9126 review (p13). `Fill` stores a closure capturing the parameter `k` into
//!   `box.F`, awaits, then writes `k` twice (+1000, +1). The closure shares `k`, so `box.F(5, 2)` = 5 -
//!   2 + (i + 1001); summed over i = 0..199: 200 x 1004 + 19900 = 220700. Value-only; the leak the fix
//!   removed is not asserted (the corpus has no leak leg), so green on both arms.
module Conf;
type Fn := (int, int) -> int;
public class Box
  public F: Fn := lambda(x: int, y: int) => 0;
  public G: Fn := lambda(x: int, y: int) => 0;
end Box;

public class C
  public static async method Fill(box: Box, k: int): Task
    box.F := lambda(x: int, y: int) => x - y + k;
    await Task.Delay(1);
    k := k + 1000;
    await Task.Delay(0);
    k := k + 1;
  end Fill;
  public static async method Main(): Task<int>
    var total := 0;
    for var i in 0 ..< 200 do
      using var box: Box := new Box();
      await Fill(box, i);
      total := total + box.F(5, 2);
    end for;
    Console.WriteLine($"{total}");
    return 0;
  end Main;
end C;
