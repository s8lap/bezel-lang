//! case: s28_9_9861
//! doc: spec
//! section: 28.9
//! title: Aliases of a generically added function element stay callable after Clear
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 28.11
//! expect: run
//! stdout: 800
//! notes: Harvested from the #9320 review (r02_alias_after_clear_9327). The element reached the array
//!   through a generic `Add`; a nullable alias and a non-null alias are taken from `arr[0]` (an
//!   indexer-get on a shared-reference slot is a "reference out", 28.11 table), then `Clear()`
//!   removes the slot's reference. Each alias holds its own reference of the runtime-managed
//!   function value, so both stay callable: 2 + 2 per round, 200 rounds = 800.
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
        G.AddTo(arr, f);
      end;
      var g: (() -> int)? := arr[0];
      var h: () -> int := arr[0];
      arr.Clear();
      if g <> null then
        acc := acc + g() + h();
      end if;
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
