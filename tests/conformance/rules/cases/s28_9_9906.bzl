//! case: s28_9_9906
//! doc: spec
//! section: 28.9
//! title: a generic swap helper returns the displaced closure and it stays callable
//! rule: Runtime management governs the *function value itself*
//! cites: 28.9, 28.11, 14.1
//! expect: run
//! stdout: 2200
//! issue: 9112
//! notes: Harvested from the #9104/#9112 review (r03_external_generic_swap). `Swap2` returns the displaced
//!   function value; it is a value the caller now holds, so `o()` is 2 and the field's `b.item()` is
//!   9: 11 per pass, 2200 over 200 passes.
module Conf;

public class Box<T>
  public item: T;

  public constructor(x: T)
    item := x;
  end constructor;

  public method Get(): T
    return item;
  end Get;

  public method Set(x: T)
    item := x;
  end Set;
end Box;

public class C
  public static method Swap2<T>(b: Box<T>, x: T): T
    var old := b.Get();
    b.Set(x);
    return old;
  end Swap2;

  public static method Main()
    var total := 0;
    for var i in 0 ..< 200 do
      using var b := new Box<() -> int>(lambda() => 2);
      var o := Swap2<() -> int>(b, lambda() => 9);
      total := total + o() + b.item();
    end for;
    Console.WriteLine(total);
  end Main;
end C;
