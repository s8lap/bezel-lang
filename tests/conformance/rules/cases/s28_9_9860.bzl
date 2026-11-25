//! case: s28_9_9860
//! doc: spec
//! section: 28.9
//! title: A function-typed local stays callable after the array it was added to is disposed
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 28.11
//! expect: run
//! stdout: 800
//! notes: Harvested from the #9320 review (q06_arr_dies_first). A function value is runtime-managed
//!   (28.9), and an `Array<() -> int>` slot is a shared reference (28.11), so disposing the array
//!   never invalidates the caller's own local. Trace per round: the closure returns 2; the array
//!   is read inside its block (2), disposed at the block's end, and the local is called after it
//!   (2); 200 rounds of 4 = 800.
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
      var f: () -> int := lambda() => 2;
      do
        using var arr: Array<() -> int> := new Array<() -> int>();
        G.AddTo(arr, f);
        acc := acc + arr[0]();
      end;
      acc := acc + f();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
