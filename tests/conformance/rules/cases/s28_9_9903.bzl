//! case: s28_9_9903
//! doc: spec
//! section: 28.9
//! title: a field closure passed to a generic Box<T> constructor stays callable after the box is disposed
//! rule: Runtime management governs the *function value itself*
//! cites: 28.9, 28.11, 14.1
//! expect: run
//! stdout: 603
//! issue: 9112
//! notes: Harvested from the #9104/#9112 review (v19_g09_without_set). The box shares the field's function
//!   value (28.9) and its disposal must not free it: 3 per pass (600) plus a final `keep.cb()` (3) is
//!   603.
module Conf;

public class H
  public cb: () -> int := lambda() => 3;
end H;

public class Box<T>
  public item: T;

  public constructor(x: T)
    item := x;
  end constructor;
end Box;

public class C
  public static method Main()
    var total := 0;
    var keep := new H();
    for var i in 0 ..< 200 do
      using var b := new Box<() -> int>(keep.cb);
      total := total + b.item();
    end for;
    total := total + keep.cb();
    Console.WriteLine(total);
  end Main;
end C;
