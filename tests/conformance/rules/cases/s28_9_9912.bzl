//! case: s28_9_9912
//! doc: spec
//! section: 28.9
//! title: A nullable function alias assigned on one branch survives the array being cleared
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 28.11
//! expect: run
//! stdout: 300
//! notes: Harvested from the #9327 review (r43-rev-c75-9327). An alias of an Array element of function
//!   type is a copy of a runtime-managed reference (28.11 slot table: shared-reference slots,
//!   `Clear()` removes all), so it stays callable after the array element is cleared. Output-only:
//!   the pre-fix compiler's use-after-free need not show on stdout. Even i assigns the alias (100 x
//!   2), odd i leaves it null (100 x 1): 300.
module Conf;
import System.Collections;

public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      using var arr: Array<(() -> int)?> := new Array<(() -> int)?>();
      arr.Add(lambda() => 2);
      var g: (() -> int)? := null;
      if i mod 2 = 0 then
        g := arr[0];
      end if;
      arr.Clear();
      if g <> null then
        acc := acc + g!();
      else
        acc := acc + 1;
      end if;
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
