//! case: l4_19_9302
//! doc: stdlib
//! section: 4.19
//! title: FrozenArray Length is an int in a local, an int argument, an arrow-bodied method and a loop bound
//! rule: public readonly property Length: int;
//! cites: 4.19, 27.3
//! expect: run
//! output: 7
//!   30
//!   3
//!   14
//! notes: Harvested from the #9185 review (p1, second half) and the accepting twin of l4_19_9304. Source
//!   [7, 2, 5]; `Length` is 3. `n := f.Length` is an int, so `n + 4` is 7; `Take(f.Length)` binds an int
//!   parameter, 3 * 10 = 30; `Len(f)` is an arrow-bodied method returning `f.Length`, 3; the `while` runs
//!   while `i < f.Length`, summing 7 + 2 + 5 = 14.
module Conf;
public class Program
  public static method Take(n: int): int
    return n * 10;
  end Take;
  public static method Len(f: FrozenArray<int>): int => f.Length;
  public static method Main(): int
    using var s := new Array<int>();
    s.Add(7); s.Add(2); s.Add(5);
    using var f := FrozenArray<int>.From(s);
    var n := f.Length;
    Console.WriteLine(n + 4);
    Console.WriteLine(Take(f.Length));
    Console.WriteLine(Len(f));
    var sum := 0;
    var i := 0;
    while i < f.Length do
      sum += f[i];
      i += 1;
    end while;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
