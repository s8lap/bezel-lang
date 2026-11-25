//! case: s27_5_8913
//! doc: spec
//! section: 27.5
//! title: a parameter written after an await inside a loop is seen by the closure that captured it
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice versa.
//! cites: 27.5, 30
//! expect: run
//! output: 20500
//! issue: 3466
//! notes: Known divergence, pinned to #3466 (reviewer comment on #3466 for the #9126 review, p20). Each of
//!   the three loop passes awaits, stores a closure capturing `k`, then does `k := k + 1`; all closures
//!   share the one `k`, so the last-stored closure returns i + 3 and `box.F(5, 2)` = i + 3; summed:
//!   19900 + 600 = 20500. The compiler prints 20100: a suspension lowered before the capture site
//!   inside a loop does not carry the parameter's cell holder, so the write is lost. Bank as `fail
//!   #3466`.
module Conf;
type Fn := (int, int) -> int;
public class Box
  public F: Fn := lambda(x: int, y: int) => 0;
end Box;

public class C
  public static async method Fill(box: Box, k: int): Task
    for var j in 0 ..< 3 do
      await Task.Delay(1);
      box.F := lambda(x: int, y: int) => k;
      k := k + 1;
    end for;
    await Task.Delay(0);
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
