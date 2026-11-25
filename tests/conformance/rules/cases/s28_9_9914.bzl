//! case: s28_9_9914
//! doc: spec
//! section: 28.9
//! title: A nullable function alias read inside a generic method survives the array being cleared
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 28.11, 14.1
//! expect: run
//! stdout: 800
//! notes: Harvested from the #9327 review (r43-rev-c75-9327). An alias of an Array element of function
//!   type is a copy of a runtime-managed reference (28.11 slot table: shared-reference slots,
//!   `Clear()` removes all), so it stays callable after the array element is cleared. Output-only:
//!   the pre-fix compiler's use-after-free need not show on stdout. Each iteration adds 2 (int
//!   instantiation) + 2 ("ab".Length): 4 x 200 = 800.
module Conf;
import System.Collections;

public class Program
  public static method First<T>(a: Array<(() -> T)?>): T
    var g := a[0];
    a.Clear();
    return g!();
  end First;

  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      using var ai: Array<(() -> int)?> := new Array<(() -> int)?>();
      ai.Add(lambda() => 2);
      using var as2: Array<(() -> string)?> := new Array<(() -> string)?>();
      as2.Add(lambda() => "ab");
      acc := acc + Program.First<int>(ai) + Program.First<string>(as2).Length;
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
