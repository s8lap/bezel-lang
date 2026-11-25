//! case: s28_3_8_8902
//! doc: spec
//! section: 28.3.8
//! title: a closure swapped into a function-typed field via `ref` arguments stays live at an `Object` param
//! rule: carries the empty set: those holders, every position declared `Object`, and every position a
//!   generic body declares at a type parameter
//! cites: 28.3.8, 22.3, 28.9
//! expect: run
//! output: 1800
//! notes: Pins #9092 (the `Swap(ref h.F, ref g)` variant). `Swap` exchanges the field with the local `g`
//!   (`lambda() => 9`), so `h.F` now holds the capture-free `lambda() => 9`; it is passed at `Keep`'s
//!   `Object` parameter and the holder dies. 9 per iteration, 200 iterations: 1800. The holder `Hold`
//!   itself leaks under the ASan leak leg (#9173, `ref` argument over a function-typed field); that
//!   leak is a separate open tracker and is not asserted here.
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
  public static method Swap(ref a: () -> int, ref b: () -> int)
    var t: () -> int := a;
    a := b;
    b := t;
  end Swap;
  public static method Via(rg: Reg2)
    var h := new Hold();
    var g: () -> int := lambda() => 9;
    Swap(ref h.F, ref g);
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
