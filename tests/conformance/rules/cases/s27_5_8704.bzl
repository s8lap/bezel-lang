//! case: s27_5_8704
//! doc: spec
//! section: 27.5
//! title: a closure kept in a field by an iterator body reads its captured parameter after the
//!   iterator ends
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter
//!   excepted (below).
//! cites: 27.5, 28.9, 28.3.9
//! expect: run
//! stdout: 21100
//! notes: Pins #9086 (iterator frame). `Gen` keeps `b` (captures the parameter `k`) in `box.F`
//!   before its first yield and reassigns the local after it. Each iteration of `Main` adds
//!   the two yielded values (1 + 2 = 3), then box.F(5, 2) = 5 - 2 + i = 3 + i: 6 + i per
//!   iteration; summed over i = 0..199: 1200 + 19900 = 21100. No leak assertion in the corpus, which has
//!   no leak leg: #9126 (an iterator frame's capture cell was never released) is fixed by e97a8efbc and
//!   pinned by the gtest capture_cell_released_in_async_iterator_frame_9126_test.cpp; this row pins only
//!   the value.
module Conf;
type Fn := (int, int) -> int;
public class Box
  public F: Fn := lambda(x: int, y: int) => 0;
end Box;

public class C
  public static method Gen(k: int, box: Box): IEnumerable<int>
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
    var run := lambda(z: int): int => do
      var c: Fn := lambda(x: int, y: int) => x + z;
      c := lambda(x: int, y: int) => 0;
      return 0;
    end;
    _ := run(k);
    box.F := b;
    yield return 1;
    b := g;
    yield return 2;
  end Gen;
  public static method Main(): int
    var total := 0;
    for var i in 0 ..< 200 do
      using var box: Box := new Box();
      for var v in C.Gen(i, box) do total := total + v; end for;
      total := total + box.F(5, 2);
    end for;
    Console.WriteLine($"{total}");
    return 0;
  end Main;
end C;
