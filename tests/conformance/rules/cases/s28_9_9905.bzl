//! case: s28_9_9905
//! doc: spec
//! section: 28.9
//! title: an alias read out of a Box<T> field stays callable after the field is overwritten
//! rule: Runtime management governs the *function value itself*
//! cites: 28.9, 28.11, 14.1
//! expect: run
//! stdout: 12
//! issue: 9112
//! notes: Harvested from the #9104/#9112 review (r01e_single_shot). `r` holds the first function value;
//!   overwriting the field does not end it (28.9: function values are values). r() is 2, b.item() is
//!   7, keep.cb() is 3: 12.
module Conf;

public class H
  public cb: () -> int := lambda() => 3;
end H;

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
  public static method Main()
    var total := 0;
    var keep := new H();
    using var b := new Box<() -> int>(lambda() => 2);
    var r := b.Get();
    b.Set(lambda() => 7);
    total := total + r() + b.item();
    total := total + keep.cb();
    Console.WriteLine(total);
  end Main;
end C;
