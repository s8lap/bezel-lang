//! case: s27_5_8705
//! doc: spec
//! section: 27.5
//! title: a closure kept in a field by an async body reads its captured parameter after the await
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter
//!   excepted (below).
//! cites: 27.5, 28.9, 30
//! expect: run
//! stdout: 20500
//! notes: Pins #9086 (async frame). `Fill` keeps `b` (captures the parameter `k`) in `box.F`,
//!   awaits, then reassigns the local. After the awaited call, box.F(5, 2) = 5 - 2 + i = 3 +
//!   i; summed over i = 0..199: 600 + 19900 = 20500. No leak assertion in the corpus, which has no
//!   leak leg: #9126 (an async frame's capture cell was never released) is fixed by e97a8efbc and pinned
//!   by the gtest capture_cell_released_in_async_iterator_frame_9126_test.cpp; this row pins only the value.
module Conf;
type Fn := (int, int) -> int;
public class Box
  public F: Fn := lambda(x: int, y: int) => 0;
end Box;

public class C
  public static async method Fill(box: Box, k: int): Task
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
    var run := lambda(z: int): int => do
      var c: Fn := lambda(x: int, y: int) => x + z;
      c := lambda(x: int, y: int) => 0;
      return 0;
    end;
    _ := run(k);
    box.F := b;
    await Task.Delay(0);
    b := g;
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
