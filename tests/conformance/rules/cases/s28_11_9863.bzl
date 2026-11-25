//! case: s28_11_9863
//! doc: spec
//! section: 28.11
//! title: A generic Add of a function value survives return, continue and break out of the frame
//! rule: The slot kind alone determines what every element operation does
//! cites: 28.11, 28.9
//! expect: run
//! stdout: 520
//! notes: Harvested from the #9320 review (r04_early_exits; the review's `break` at i = 150 was
//!   unreachable behind `continue` at every multiple of 5, so the break index is 151 here). The
//!   stored closure is read back through the slot on every exit path. Trace: the loop runs i in
//!   0..150 (break at 151) and skips multiples of 5, leaving 120 rounds; 40 of them are multiples of
//!   3 and `Work` returns early with arr[0]() = 2, the other 80 return 2 + 2 = 4; each round adds the
//!   outer closure's 1 (40 * 3 + 80 * 5 = 520).
module Conf;
import System.Collections;

public class G
  public static method AddTo<T>(xs: Array<T>, x: T)
    xs.Add(x);
  end AddTo;
end G;

public class Program
  public static method Work(i: int): int
    using var arr: Array<() -> int> := new Array<() -> int>();
    var f: () -> int := lambda() => 2;
    G.AddTo(arr, f);
    if i mod 3 = 0 then
      return arr[0]();
    end if;
    G.AddTo(arr, f);
    return arr[0]() + arr[1]();
  end Work;

  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      var f: () -> int := lambda() => 1;
      using var arr: Array<() -> int> := new Array<() -> int>();
      G.AddTo(arr, f);
      if i mod 5 = 0 then
        continue;
      end if;
      if i = 151 then
        break;
      end if;
      acc := acc + Work(i) + arr[0]();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
