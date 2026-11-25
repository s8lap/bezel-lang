//! case: s28_11_9861
//! doc: spec
//! section: 28.11
//! title: Generic Add and Insert of function values, then RemoveAt and Clear, keep the survivors callable
//! rule: The slot kind alone determines what every element operation does
//! cites: 28.11, 28.9
//! expect: run
//! stdout: 1800
//! notes: Harvested from the #9320 review (q02_insert_removeat_clear). Shared-reference slots (28.11
//!   table): `Insert` stores the reference, `RemoveAt` removes the reference, `Clear()` removes all.
//!   Trace per round, f = 2 and g = 3: AddTo f gives [f]; InsertTo g at 0 gives [g, f]; read 3 + 2 = 5;
//!   RemoveAt(0) gives [f]; read 2 (7); AddTo g gives [f, g]; Clear() gives []; AddTo f gives [f] and
//!   the read after the block adds 2 (9). 200 rounds = 1800.
module Conf;
import System.Collections;

public class G
  public static method AddTo<T>(xs: Array<T>, x: T)
    xs.Add(x);
  end AddTo;

  public static method InsertTo<T>(xs: Array<T>, x: T)
    xs.Insert(0, x);
  end InsertTo;
end G;

public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      using var arr: Array<() -> int> := new Array<() -> int>();
      do
        var f: () -> int := lambda() => 2;
        var g: () -> int := lambda() => 3;
        G.AddTo(arr, f);
        G.InsertTo(arr, g);
        acc := acc + arr[0]() + arr[1]();
        arr.RemoveAt(0);
        acc := acc + arr[0]();
        G.AddTo(arr, g);
        arr.Clear();
        G.AddTo(arr, f);
      end;
      acc := acc + arr[0]();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
