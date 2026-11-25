//! case: s28_3_8_8735
//! doc: spec
//! section: 28.3.8
//! title: a capture-free closure read from a function-typed field is admitted at an Object
//!   parameter
//! rule: carries the empty set: those holders, every position declared `Object`, and every position
//!   a generic body declares at a type parameter
//! cites: 28.3.8, 28.9
//! expect: run
//! stdout: 200
//! notes: The pin for #9092 (reviewer r43-rev-h10 replacement for 8735). `Via` reads `h.F` out of a
//!   function-typed field and passes it at the `Object` parameter of `Keep`, then the holder `h`
//!   is disposed when `Via` returns; the kept `Object` box must still hold a live closure, so
//!   the later `RunInt` call (1 per iteration, 200 iterations: 200) is valid. Segfaults on the
//!   compiler before db3dafb3b.
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
  public static method Via(rg: Reg2)
    var h := new Hold();
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
