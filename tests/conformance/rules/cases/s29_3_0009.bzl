//! case: s29_3_0009
//! doc: spec
//! section: 29.3
//! title: each null-conditional Count and Sum on a nullable iterator local reruns the iterator body
//! rule: "An iterator method's return type is `IEnumerable<T>` (re-iterable: each call to
//!   `GetEnumerator()` runs the body again from the start)." Each of `c?.Count()` and
//!   `c?.Sum()` enumerates a fresh sequence, so over `Mk(4)` yielding 1 to 4 each pair
//!   contributes 4 + 10 = 14, and 200 iterations total 2800. A single-use sequence
//!   would give 800: the `Sum` would see the already-consumed source. (§29.3 with §7.5, v0.350.)
//! cites: 29.3, 7.5
//! expect: run
//! output: 2800
module Conf;

import System;
import System.Linq;

public class P
  public static method Mk(n: int): IEnumerable<int>
    for var i in 1..n do
      yield return i;
    end;
  end Mk;

  public static method Main()
    var s := 0;
    for var i in 1..200 do
      var c: IEnumerable<int>? := Mk(4);
      s := s + (c?.Count() ?? -1) + (c?.Sum() ?? 0);
    end;
    Console.WriteLine(s);
  end Main;
end P;
