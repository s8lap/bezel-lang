//! case: s27_5_8908
//! doc: spec
//! section: 27.5
//! title: two closures sharing an async parameter's cell, one escaping, read it after the frame ends
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted
//!   (below).
//! cites: 27.5, 28.9, 30
//! expect: run
//! output: 60300
//! notes: Harvested from the #9126 review (p10). Two closures capture `k`: a local one invoked inside the
//!   frame (`local(2, 0)` = 2k) and one stored in `box.F` that outlives the frame (`box.F(5, 2)` = 3 +
//!   k). Per iteration 2i + 3 + i; summed over i = 0..199: 3 x 19900 + 600 = 60300. Value-only (green
//!   on both arms).
module Conf;
type Fn := (int, int) -> int;
public class Box
  public F: Fn := lambda(x: int, y: int) => 0;
  public G: Fn := lambda(x: int, y: int) => 0;
end Box;

public class C
  public static async method Fill(box: Box, k: int): Task<int>
    var local: Fn := lambda(x: int, y: int) => x * k;
    box.F := lambda(x: int, y: int) => x - y + k;
    await Task.Delay(1);
    var v := local(2, 0);
    local := lambda(x: int, y: int) => 0;
    return v;
  end Fill;
  public static async method Main(): Task<int>
    var total := 0;
    for var i in 0 ..< 200 do
      using var box: Box := new Box();
      total := total + await Fill(box, i);
      total := total + box.F(5, 2);
    end for;
    Console.WriteLine($"{total}");
    return 0;
  end Main;
end C;
