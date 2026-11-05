//! case: s26_7_1415
//! doc: spec
//! section: 26.7
//! title: an extension may store a receiver-sourced child borrow into another object's field
//! rule: borrows sourced from a parameter that is **not `owned`** — a plain or `scoped` parameter —
//!   or from the receiver are admissible; scope-bound locals and temporaries are rejected
//! cites: 26.7, 28.3
//! expect: run
//! stdout: 1
module Conf;

public class Holder
  public o: Object?;

  public constructor()
    o := null;
  end constructor;
end Holder;

public class Box
  public child: Box?;

  public constructor()
    child := null;
  end constructor;

  public method Self(): Box => this;

  public method Child(): Box? => this.child;

  public method Pick(c: bool): Box
    if c then return this; end;
    return this.child!;
  end Pick;
end Box;

public extension Box
  public method M1(h: Holder, c: bool): int
    h.o := this.Child();
    return 1;
  end M1;
end extension;

public class Program
  public static method Main()
    using var h := new Holder();
    using var b := new Box();
    Console.WriteLine(b.M1(h, true));
  end Main;
end Program;
