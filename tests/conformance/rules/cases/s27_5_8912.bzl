//! case: s27_5_8912
//! doc: spec
//! section: 27.5
//! title: an async parameter captured on only some paths is read by the escaping closure after early returns
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted
//!   (below).
//! cites: 27.5, 28.9, 30
//! expect: run
//! output: 7434
//! notes: Harvested from the #9126 review (p03). `Fill` returns 7 early when k mod 3 = 0, captures `k` into
//!   `box.F` only when k is even (returning 1 after the await when k mod 4 = 0), and otherwise returns
//!   2. `F` keeps its default `0` unless set. Sums over 0..199: returns 469 + 233, F contributions 6732
//!   = 7434. Value-only (green on both arms).
module Conf;
type Fn := (int, int) -> int;
public class Box
  public F: Fn := lambda(x: int, y: int) => 0;
  public G: Fn := lambda(x: int, y: int) => 0;
end Box;

public class C
  public static async method Fill(box: Box, k: int): Task<int>
    await Task.Delay(0);
    if k mod 3 = 0 then
      return 7;
    end if;
    if k mod 2 = 0 then
      box.F := lambda(x: int, y: int) => x - y + k;
      await Task.Delay(1);
      if k mod 4 = 0 then
        return 1;
      end if;
    end if;
    await Task.Delay(0);
    return 2;
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
