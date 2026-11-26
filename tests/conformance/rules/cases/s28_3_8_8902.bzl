//! case: s28_3_8_8902
//! doc: spec
//! section: 28.3.8
//! title: a non-generic Swap of two function-typed ref parameters is rejected in its own body
//! rule: A read carrying the unannounced source may be invoked, bound to a local or passed at a plain
//!   function-typed parameter, but not stored into a holder (the `ref` or `out` location itself and a
//!   sibling one included), passed at a set-erasing position or returned from the enclosing method.
//! cites: 28.3.8, 22.3, 28.9
//! expect: reject
//! diagnostic: unannounced
//! notes: Pins #9092's `Swap(ref h.F, ref g)` shape; v0.361 (#9093) flips it from run to reject.
//!   `Swap`'s `a := b` stores `b`'s entry value, an unannounced source, into the sibling `ref` location,
//!   so the callee is rejected whatever its callers pass. The field route stays expressible with the
//!   generic `Swap<T>(ref a: T, ref b: T)` (s28_3_8_8601).
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
