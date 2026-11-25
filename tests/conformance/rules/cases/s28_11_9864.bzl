//! case: s28_11_9864
//! doc: spec
//! section: 28.11
//! title: A direct Add and two generic Adds of one function value give independent shared references
//! rule: The slot kind alone determines what every element operation does
//! cites: 28.11, 28.9
//! expect: run
//! stdout: 800
//! notes: Harvested from the #9320 review (r07_direct_and_generic_same_f). Every store is a shared
//!   reference, whichever spelling made it (`arr.Add(f)` directly, `G.AddTo(arr, f)` through the
//!   generic helper), and `RemoveAt` removes one reference. A double count would over-release the
//!   closure and fault. Trace per round: Add gives [f]; two AddTo give [f, f, f]; RemoveAt(1) gives
//!   [f, f]; the read after the block is 2 + 2 = 4; 200 rounds = 800.
module Conf;
import System.Collections;

public class G
  public static method AddTo<T>(xs: Array<T>, x: T)
    xs.Add(x);
  end AddTo;
end G;

public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      using var arr: Array<() -> int> := new Array<() -> int>();
      do
        var f: () -> int := lambda() => 2;
        arr.Add(f);
        G.AddTo(arr, f);
        G.AddTo(arr, f);
        arr.RemoveAt(1);
      end;
      acc := acc + arr[0]() + arr[1]();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
