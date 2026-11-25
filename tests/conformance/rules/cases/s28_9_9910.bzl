//! case: s28_9_9910
//! doc: spec
//! section: 28.9
//! title: A nullable function alias of an array element survives an early return
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 28.11
//! expect: run
//! stdout: 700
//! notes: Harvested from the #9327 review (r43-rev-c75-9327). An alias of an Array element of function
//!   type is a copy of a runtime-managed reference (28.11 slot table: shared-reference slots,
//!   `Clear()` removes all), so it stays callable after the array element is cleared. Output-only:
//!   the pre-fix compiler's use-after-free need not show on stdout. Even i returns 5 (100 x 5), odd i
//!   clears then calls the alias (100 x 2): 700.
module Conf;
import System.Collections;

public class Program
  public static method Run(i: int): int
    using var arr: Array<(() -> int)?> := new Array<(() -> int)?>();
    arr.Add(lambda() => 2);
    var g := arr[0];
    if i mod 2 = 0 then
      return 5;
    end if;
    arr.Clear();
    return g!();
  end Run;

  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      acc := acc + Program.Run(i);
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
