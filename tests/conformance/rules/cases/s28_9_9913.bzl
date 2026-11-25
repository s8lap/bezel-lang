//! case: s28_9_9913
//! doc: spec
//! section: 28.9
//! title: A returned nullable function alias outlives the cleared array it was read from
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 28.11
//! expect: run
//! stdout: 400
//! notes: Harvested from the #9327 review (r43-rev-c75-9327). An alias of an Array element of function
//!   type is a copy of a runtime-managed reference (28.11 slot table: shared-reference slots,
//!   `Clear()` removes all), so it stays callable after the array element is cleared. Output-only:
//!   the pre-fix compiler's use-after-free need not show on stdout. 200 x 2 = 400.
module Conf;
import System.Collections;

public class Program
  public static method Pick(): () -> int
    using var arr: Array<(() -> int)?> := new Array<(() -> int)?>();
    arr.Add(lambda() => 2);
    var g := arr[0];
    arr.Clear();
    return g!;
  end Pick;

  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      var f := Program.Pick();
      acc := acc + f();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
