//! case: s28_9_9911
//! doc: spec
//! section: 28.9
//! title: A nullable function alias of an array element survives continue and break
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 28.11
//! expect: run
//! stdout: 264
//! notes: Harvested from the #9327 review (r43-rev-c75-9327). An alias of an Array element of function
//!   type is a copy of a runtime-managed reference (28.11 slot table: shared-reference slots,
//!   `Clear()` removes all), so it stays callable after the array element is cleared. Output-only:
//!   the pre-fix compiler's use-after-free need not show on stdout. i in 0..198 not divisible by 3 is
//!   132 iterations x 2 = 264; i = 199 breaks before the add.
module Conf;
import System.Collections;

public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      using var arr: Array<(() -> int)?> := new Array<(() -> int)?>();
      arr.Add(lambda() => 2);
      var g := arr[0];
      if i mod 3 = 0 then
        continue;
      end if;
      if i = 199 then
        break;
      end if;
      arr.Clear();
      acc := acc + g!();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
