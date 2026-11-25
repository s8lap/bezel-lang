//! case: s27_5_8910
//! doc: spec
//! section: 27.5
//! title: an iterator's escaping closure over its parameter reads it after `yield break` or falling off
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted
//!   (below).
//! cites: 27.5, 28.9, 29
//! expect: run
//! output: 23933
//! notes: Harvested from the #9126 review (p17; the iterator-frame sibling of the async rows). `Gen` stores
//!   a closure capturing `k` into `box.F` when k is even and another into `box.G` after the first yield
//!   unless k mod 3 = 0 (then `yield break`). Per iteration: the yields (1, plus 2 unless k mod 3 = 0),
//!   F(5, 2) = 3 + k for even k, G(0, 0) = k unless k mod 3 = 0. Sums over 0..199: 466 + 10200 + 13267
//!   = 23933. Value-only.
module Conf;
type Fn := (int, int) -> int;
public class Box
  public F: Fn := lambda(x: int, y: int) => 0;
  public G: Fn := lambda(x: int, y: int) => 0;
end Box;

public class C
  public static method Gen(k: int, box: Box): IEnumerable<int>
    if k mod 2 = 0 then
      box.F := lambda(x: int, y: int) => x - y + k;
    end if;
    yield return 1;
    if k mod 3 = 0 then
      yield break;
    end if;
    box.G := lambda(x: int, y: int) => k;
    yield return 2;
  end Gen;
  public static method Main(): int
    var total := 0;
    for var i in 0 ..< 200 do
      using var box: Box := new Box();
      for var v in C.Gen(i, box) do total := total + v; end for;
      total := total + box.F(5, 2) + box.G(0, 0);
    end for;
    Console.WriteLine($"{total}");
    return 0;
  end Main;
end C;
