//! case: s27_5_8909
//! doc: spec
//! section: 27.5
//! title: two escaping closures over one async parameter, stored in two fields, both read it after the frame
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted
//!   (below).
//! cites: 27.5, 28.9, 30
//! expect: run
//! output: 80200
//! notes: Harvested from the #9126 review (p11). `Fill` stores one closure capturing `k` in `box.F` and,
//!   after an await, another in `box.G`. `Main` then calls `F(5, 2)` = 3 + i and `G(2, 0)` = 2i,
//!   replaces `F`, and calls `G(1, 0)` = i: 3 + 4i per iteration; summed: 600 + 4 x 19900 = 80200.
//!   Value-only (green on both arms).
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
    box.G := lambda(x: int, y: int) => x * k;
  end Fill;
  public static async method Main(): Task<int>
    var total := 0;
    for var i in 0 ..< 200 do
      using var box: Box := new Box();
      await Fill(box, i);
      total := total + box.F(5, 2) + box.G(2, 0);
      box.F := lambda(x: int, y: int) => 0;
      total := total + box.G(1, 0);
    end for;
    Console.WriteLine($"{total}");
    return 0;
  end Main;
end C;
