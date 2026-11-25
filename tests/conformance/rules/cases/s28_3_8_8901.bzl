//! case: s28_3_8_8901
//! doc: spec
//! section: 28.3.8
//! title: a closure written to a function-typed field via a `ref` argument stays live at an `Object`
//!   parameter
//! rule: carries the empty set: those holders, every position declared `Object`, and every position a
//!   generic body declares at a type parameter
//! cites: 28.3.8, 22.3, 28.9
//! expect: run
//! output: 1800
//! notes: Pins #9092 (the `Reset(ref h.F)` variant; the plain-read variant is s28_3_8_8735). `Reset` writes
//!   the capture-free `lambda() => 9` into `h.F` through a `ref` argument; `Via` then passes `h.F` at
//!   the `Object` parameter of `Keep` and the holder `h` dies when `Via` returns. The kept box must
//!   still hold a live closure, so each `RunInt` returns 9 (200 iterations: 1800). Heap-use-after-free
//!   / SEGV on the compiler before db3dafb3b. The holder `Hold` itself leaks under the ASan leak leg
//!   (#9173, `ref` argument over a function-typed field); that leak is a separate open tracker and is
//!   not asserted here.
module Conf;
public class Reg2
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
  public method RunInt(): int
    if _o is () -> int then return (_o as () -> int)!(); end if;
    return 7;
  end RunInt;
end Reg2;

public class Hold
  public F: () -> int := lambda() => 1;
  public constructor() end constructor;
end Hold;

public class Program
  public static method Reset(ref x: () -> int) x := lambda() => 9; end Reset;
  public static method Via(rg: Reg2)
    var h := new Hold();
    Reset(ref h.F);
    rg.Keep(h.F);
  end Via;
  public static method Main(): int
    using var reg := new Reg2();
    var n: int := 0;
    for var i in 0 ..< 200 do
      Via(reg);
      n := n + reg.RunInt();
    end for;
    Console.WriteLine($"{n}");
    return 0;
  end Main;
end Program;
