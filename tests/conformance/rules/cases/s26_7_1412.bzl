//! case: s26_7_1412
//! doc: spec
//! section: 26.7
//! title: an extension over Box that stores nothing from its receiver is admitted
//! rule: The receiver is always a borrow; extensions cannot store `this`, return it as `owned`, or
//!   dispose it.
//! cites: 26.7
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
    h.o := null;
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
