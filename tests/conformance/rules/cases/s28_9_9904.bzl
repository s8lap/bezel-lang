//! case: s28_9_9904
//! doc: spec
//! section: 28.9
//! title: a closure from a loop-local object passed to a generic Box<T> is callable and the source
//!   survives
//! rule: Runtime management governs the *function value itself*
//! cites: 28.9, 28.11, 14.1
//! expect: run
//! stdout: 603
//! issue: 9112
//! notes: Harvested from the #9104/#9112 review (v20_v15_without_set). Same shape as s28_9_9903 with a
//!   fresh source object per pass: 3 per pass (600) plus `keep.cb()` (3) is 603.
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
      var hh := new H();
      using var b := new Box<() -> int>(hh.cb);
      total := total + b.item();
    end for;
    total := total + keep.cb();
    Console.WriteLine(total);
  end Main;
end C;
