//! case: s28_9_9902
//! doc: spec
//! section: 28.9
//! title: a closure read out of a generic Box<T> field into a second holder stays callable
//! rule: Runtime management governs the *function value itself*
//! cites: 28.9, 28.11, 14.1
//! expect: run
//! stdout: 800
//! issue: 9112
//! notes: Harvested from the #9104/#9112 review (v18_r13_without_set). Both holders hold the one function
//!   value (runtime-managed, 28.9), so each pass calls it twice: 2 + 2 = 4, 800 over 200 passes.
module Conf;

public class Box<T>
  public item: T;

  public constructor(x: T)
    item := x;
  end constructor;

  public method Get(): T
    return item;
  end Get;
end Box;

public class Keep<T>
  public held: T;

  public constructor(b: Box<T>)
    held := b.Get();
  end constructor;
end Keep;

public class C
  public static method Main()
    var total := 0;
    for var i in 0 ..< 200 do
      using var b := new Box<() -> int>(lambda() => 2);
      using var k := new Keep<() -> int>(b);
      total := total + k.held() + b.item();
    end for;
    Console.WriteLine(total);
  end Main;
end C;
